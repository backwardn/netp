#include "thread.h"
#include <stdlib.h>

ItBlock::ItBlock( int size )
:
	size(size)
{
}

ItWriter::ItWriter()
:
	thread(0),
	queue(0),
	id(-1),
	head(0), tail(0),
	hoff(0), toff(0),
	toSend(0)
{
}

ItQueue::ItQueue( int blockSz )
:
	head(0), tail(0),
	blockSz(blockSz)
{
	pthread_mutex_init( &mutex, 0 );
	pthread_cond_init( &cond, 0 );

	free = 0;
}

ItWriter *ItQueue::registerWriter( Thread *thread )
{
	ItWriter *writer = new ItWriter;
	writer->thread = thread;
	writer->queue = this;

	/* Reigster under lock. */
	pthread_mutex_lock( &mutex );

	/* Allocate an id (index into the vector of writers). */
	for ( int i = 0; i < (int)writerVect.size(); i++ ) {
		/* If there is a free spot, use it. */
		if ( writerVect[i] == 0 ) {
			writerVect[i] = writer;
			writer->id = i;
			goto set;
		}
	}

	/* No Existing index to use. Append. */
	writer->id = writerVect.size();
	writerVect.push_back( writer );

set:
	writerList.append( writer );

	pthread_mutex_unlock( &mutex );
	return writer;
}

ItBlock *ItQueue::allocateBlock()
{
	return new ItBlock( blockSz );
}

void ItQueue::freeBlock( ItBlock *block )
{
	delete block;
}

void *ItQueue::allocBytes( ItWriter *writer, int size )
{
	if ( writer->tail == 0 ) {
		/* There are no blocks. */
		writer->head = writer->tail = allocateBlock();
		writer->hoff = writer->toff = 0;
	}
	else {
		int avail = writer->tail->size - writer->toff;

		/* Move to the next block? */
		if ( size > avail ) {
			/* Move */
			ItBlock *block = allocateBlock();
			writer->tail->next = block;
			writer->tail = block;
			writer->toff = 0;
		}
	}

	void *ret = writer->tail->data + writer->toff;
	writer->toff += size;
	return ret;
}

ItHeader *ItQueue::startMessage( ItWriter *writer )
{
	/* Place the header. */
	ItHeader *header = (ItHeader*)allocBytes(
			writer, sizeof(ItHeader) );
	
	static int msgId = 1;
	header->msgId = msgId++;
	header->writerId = writer->id;
	header->length = sizeof(ItHeader);

	writer->toSend = header;

	return header;
}

void ItQueue::send( ItHeader *header )
{
	ItWriter *writer = writerVect[header->writerId];
	pthread_mutex_lock( &mutex );

	/* Put on the end of the message list. */
	if ( head == 0 )
		head = tail = writer->toSend;
	else {
		tail->next = writer->toSend;
		tail = writer->toSend;
	}

	/* Notify anyone waiting. */
	pthread_cond_broadcast( &cond );

	pthread_mutex_unlock( &mutex );
}

ItHeader *ItQueue::wait()
{
	pthread_mutex_lock( &mutex );

	while ( head == 0 )
		pthread_cond_wait( &cond, &mutex );

	ItHeader *msg = head;
	head = head->next;

	pthread_mutex_unlock( &mutex );

	msg->next = 0;
	return msg;
}

void ItQueue::release( ItHeader *header )
{
	ItWriter *writer = writerVect[header->writerId];
	int length = header->length;

	/* Skip whole blocks. */
	int remaining = writer->head->size - writer->hoff;
	while ( length >= remaining ) {
		/* Pop the block. */
		ItBlock *pop = writer->head;
		writer->head = writer->head->next;
		writer->hoff = 0;
		freeBlock( pop );

		/* Take what was left off the length. */
		length -= remaining;

		/* Remaining is the size of the next block (always starting at 0). */
		remaining = writer->head->size;
	}

	/* Final move ahead. */
	writer->hoff += length;
};

void *thread_start_routine( void *arg )
{
	Thread *thread = (Thread*)arg;
	long r = thread->start();
	return (void*)r;
}

std::ostream &operator <<( std::ostream &out, const Thread::endp & )
{
	exit( 1 );
}
