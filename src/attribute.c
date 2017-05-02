#include "attribute.h"
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/cacheflush.h>

#include "krkern.h"
#include "kckern.h"
#include "common.c"

struct kring
{
	struct kobject kobj;
};

static struct kring_ringset *head_data = 0;
static struct kctl_ringset *head_cmd = 0;

static int kring_sock_release( struct socket *sock );
static int kring_sock_create( struct net *net, struct socket *sock, int protocol, int kern );
static int kring_sock_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma );
static int kring_bind(struct socket *sock, struct sockaddr *sa, int addr_len);
static unsigned int kring_poll(struct file *file, struct socket *sock, poll_table * wait);
static int kring_setsockopt(struct socket *sock, int level, int optname, char __user * optval, unsigned int optlen);
static int kring_getsockopt(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);
static int kring_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg);
static int kring_recvmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len, int flags );
static int kring_sendmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len );

static struct proto_ops kring_ops = {
	.family = KRING,
	.owner = THIS_MODULE,

	.release = kring_sock_release,
	.bind = kring_bind,
	.mmap = kring_sock_mmap,
	.poll = kring_poll,
	.setsockopt = kring_setsockopt,
	.getsockopt = kring_getsockopt,
	.ioctl = kring_ioctl,
	.recvmsg = kring_recvmsg,
	.sendmsg = kring_sendmsg,

	/* Not used. */
	.connect = sock_no_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = sock_no_getname,
	.listen = sock_no_listen,
	.shutdown = sock_no_shutdown,
	.sendpage = sock_no_sendpage,
};

struct kring_sock
{
	struct sock sk;
	struct kring_ringset *ringset;
	int ring_id;
	enum KRING_MODE mode;
	int writer_id;
	int reader_id;
};

static inline struct kring_sock *kring_sk( const struct sock *sk )
{
	return container_of( sk, struct kring_sock, sk );
}

static struct proto kring_proto = {
	.name = "KRING",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct kring_sock),
};

static struct net_proto_family kring_family_ops = {
	.family = KRING,
	.create = kring_sock_create,
	.owner = THIS_MODULE,
};


static int kring_sock_release( struct socket *sock )
{
	struct sock *sk = sock->sk;

	if ( !sk )
		return 0;

	printk( "kring_sock_release\n" );

	sock->sk = NULL;
	sock_put( sk );
	return 0;
}

static void decon_pgoff( unsigned long pgoff, unsigned long *rid, unsigned long *region )
{
	*rid = ( pgoff & KRING_PGOFF_ID_MASK ) >> KRING_PGOFF_ID_SHIFT;
	*region = ( pgoff & KRING_PGOFF_REGION_MASK ) >> KRING_PGOFF_REGION_SHIFT;
}

static int kring_sock_mmap( struct file *file, struct socket *sock, struct vm_area_struct *vma )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	unsigned long ring_id, region;

	/* Ensure bound. */
	if ( r == 0 ) {
		printk( "kring mmap: socket not bound to ring\n" );
		return -EINVAL;
	}
	
	decon_pgoff( vma->vm_pgoff, &ring_id, &region );

	if ( ring_id > r->nrings ) {
		printk( "kring mmap: error: rid > r->nrings\n" );
		return -EINVAL;
	}

	switch ( region  ) {
		case KRING_PGOFF_CTRL: {
			printk( "mapping control region %p of ring %p-%lu\n", r->ring[ring_id].ctrl, r, ring_id );
			remap_vmalloc_range( vma, r->ring[ring_id].ctrl, 0 );
			break;
		}

		case KRING_PGOFF_DATA: {
			int i;
			unsigned long uaddr = vma->vm_start;
			printk( "mapping data region %lu of ring %p-%lu\n", uaddr, r, ring_id );
			for ( i = 0; i < KRING_NPAGES; i++ ) {
				vm_insert_page( vma, uaddr, r->ring[ring_id].pd[i].p );
				uaddr += PAGE_SIZE;
			}

			break;
		}
	}

	return 0;
}

static int validate_ring_name( const char *name )
{
	const char *p = name;
	const char *pe = name + KRING_NLEN;
	while ( 1 ) {
		/* Reached the end without validation. */
		if ( p == pe )
			return -1;

		/* Got to a valid string end. Finish. */
		if ( *p == 0 )
			return 0;

		if ( ! ( ( 'A' <= *p && *p <= 'Z' ) ||
				( 'a' <= *p && *p <= 'z' ) ||
				( '0' <= *p && *p <= '9' ) ||
				*p == '_' || *p == '-' || 
				*p == '.' ) )
		{
			/* Invalid char */
			return -1;
		}

		/* Okay, next char. */
		p += 1;
	}
}

struct kctl_ringset *kctl_find_ring( const char *name )
{
	struct kctl_ringset *r = head_cmd;
	while ( r != 0 ) {
		if ( strcmp( r->name, name ) == 0 )
			return r;

		r = r->next;
	}

	return 0;
}

static struct kring_ringset *find_ring( const char *name )
{
	struct kring_ringset *r = head_data;
	while ( r != 0 ) {
		if ( strcmp( r->name, name ) == 0 )
			return r;

		r = r->next;
	}

	return 0;
}

static int kring_allocate_reader_on_ring( struct kring_ring *ring )
{
	int reader_id;

	/* Search for a reader id that is free on the ring requested. */
	for ( reader_id = 0; reader_id < KRING_READERS; reader_id++ ) {
		if ( !ring->reader[reader_id].allocated )
			break;
	}

	if ( reader_id == KRING_READERS ) {
		/* No valid id found */
		return -EINVAL;
	}

	/* All okay. */
	ring->reader[reader_id].allocated = true;
	ring->num_readers += 1;

	return reader_id;
}

static int kring_allocate_reader_all_rings( struct kring_ringset *ringset )
{
	int i, reader_id;

	/* Search for a single reader id that is free across all the rings requested. */
	for ( reader_id = 0; reader_id < KRING_READERS; reader_id++ ) {
		for ( i = 0; i < ringset->nrings; i++ ) {
			if ( ringset->ring[i].reader[reader_id].allocated )
				goto next_id;
		}

		/* Got through all the rings, break with a valid id. */
		goto good;

		next_id: {}
	}

	/* No valid id found (exited id loop). */
	return -EINVAL;

	/* All okay. */
	good: {}
	
	/* Allocate reader ids, increas reader count. */
	for ( i = 0; i < ringset->nrings; i++ ) {
		ringset->ring[i].reader[reader_id].allocated = true;
		ringset->ring[i].num_readers += 1;
	}
	
	return reader_id;
}

static int kring_allocate_writer_on_ring( struct kring_ring *ring )
{
	int writer_id;

	/* Search for a writer id that is free on the ring requested. */
	for ( writer_id = 0; writer_id < KRING_WRITERS; writer_id++ ) {
		if ( !ring->writer[writer_id].allocated )
			break;
	}

	if ( writer_id == KRING_WRITERS ) {
		/* No valid id found */
		return -EINVAL;
	}

	/* All okay. */
	ring->writer[writer_id].allocated = true;
	ring->num_writers += 1;

	return writer_id;
}

static int kring_bind( struct socket *sock, struct sockaddr *sa, int addr_len )
{
	int reader_id = -1, writer_id = -1;
	struct kring_addr *addr = (struct kring_addr*)sa;
	struct kring_sock *krs;
	struct kring_ringset *ringset;

	if ( addr_len != sizeof(struct kring_addr) ) {
		printk("kring_bind: addr_len wrong size\n");
		return -EINVAL;
	}

	if ( validate_ring_name( addr->name ) < 0 ) {
		printk( "kring_bind: bad ring name\n" );
		return -EINVAL;
	}
	
	if ( addr->mode != KRING_READ && addr->mode != KRING_WRITE ) {
		printk( "kring_bind: bad mode, not read or write\n" );
		return -EINVAL;
	}
	
	ringset = find_ring( addr->name );
	if ( ringset == 0 ) {
		printk( "kring_bind: bad mode, not read or write\n" );
		return -EINVAL;
	}

	if ( addr->ring_id != KRING_RING_ID_ALL &&
			( addr->ring_id < 0 || addr->ring_id >= ringset->nrings ) )
	{
		printk( "kring_bind: bad ring id %d\n", addr->ring_id );
		return -EINVAL;
	}

	/* Cannot write to all rings. */
	if ( addr->mode == KRING_WRITE && addr->ring_id == KRING_RING_ID_ALL ) {
		printk( "kring_bind: cannot write to ring id ALL\n" );
		return -EINVAL;
	}

	krs = kring_sk( sock->sk );

	if ( addr->mode == KRING_WRITE ) {
		/* Find a writer ID. */
		writer_id = kring_allocate_writer_on_ring( &ringset->ring[addr->ring_id] );
		if ( writer_id < 0 )
			return writer_id;
	}
	else if ( addr->mode == KRING_READ ) {
		/* Find a reader ID. */
		if ( addr->ring_id != KRING_RING_ID_ALL ) {
			/* Reader ID for ring specified. */
			reader_id = kring_allocate_reader_on_ring( &ringset->ring[addr->ring_id] );
		}
		else {
			/* Find a reader ID that works for all rings. This can fail. */
			reader_id = kring_allocate_reader_all_rings( ringset );
		}

		if ( reader_id < 0 )
			return reader_id;

	}

	krs->ringset = ringset;
	krs->ring_id = addr->ring_id;
	krs->mode = addr->mode;
	krs->writer_id = writer_id;
	krs->reader_id = reader_id;

	return 0;
}

unsigned int kring_poll(struct file *file, struct socket *sock, poll_table * wait)
{
	printk( "kring_poll\n" );
	return 0;
}

static int kring_setsockopt(struct socket *sock, int level, int optname, char __user * optval, unsigned int optlen)
{
	printk( "kring_setsockopt\n" );
	return 0;
}

static int kring_getsockopt( struct socket *sock, int level, int optname, char __user *optval, int __user *optlen )
{
	int len;
	int val, lv = sizeof(val);
	void *data = &val;
	struct kring_sock *krs = kring_sk( sock->sk );

	if ( level != SOL_PACKET )
		return -ENOPROTOOPT;

	if ( get_user(len, optlen) )
		return -EFAULT;

	if ( len != lv )
		return -EINVAL;

	printk( "kring_getsockopt\n" );

	switch ( optname ) {
		case KR_OPT_RING_N:
			val = krs->ringset->nrings;
			break;
		case KR_OPT_READER_ID:
			val = krs->reader_id;
			break;
		case KR_OPT_WRITER_ID:
			val = krs->writer_id;
			break;
	}

	if ( put_user( len, optlen ) )
		return -EFAULT;

	if ( copy_to_user( optval, data, len) )
		return -EFAULT;
	return 0;
}

static int kring_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	printk( "kring_ioctl\n" );
	return 0;
}

static int kring_kern_avail( struct kring_ringset *r, struct kring_sock *krs )
{
	if ( krs->ring_id != KRING_RING_ID_ALL )
		return kring_avail_impl( &r->ring[krs->ring_id].control, krs->reader_id );
	else {
		int ring;
		for ( ring = 0; ring < r->nrings; ring++ ) {
			if ( kring_avail_impl( &r->ring[ring].control, krs->reader_id ) )
				return 1;
		}
		return 0;
	}
}

/* Waiting writers go to sleep with the recvmsg system call. */
static int kring_recvmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len, int flags )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	sigset_t blocked, oldset;
	int ret;
	wait_queue_head_t *wq;

	/* Allow kill, stop and the user sigs. This assumes we are operating under
	 * the genf program framework where we want to atomically unmask the user
	 * signals so we can receive genf inter-thread messages. */
	siginitsetinv( &blocked, sigmask(SIGKILL) | sigmask(SIGSTOP) |
			sigmask(SIGUSR1) | sigmask(SIGUSR2) );
	sigprocmask( SIG_SETMASK, &blocked, &oldset );

	// wq = krs->ring_id == KRING_RING_ID_ALL ? &r->reader_waitqueue 
	// : &r->ring[krs->ring_id].reader_waitqueue;

	wq = &r->reader_waitqueue;

	ret = wait_event_interruptible( *wq, kring_kern_avail( r, krs ) );

	if ( ret == -ERESTARTSYS ) {
		/* Interrupted by a signal. */
		memcpy(&current->saved_sigmask, &oldset, sizeof(oldset));
		set_restore_sigmask();
		return -EINTR;
	}

	sigprocmask(SIG_SETMASK, &oldset, NULL);
	return 0;
}

/* The sendmsg system call is used for waking up waiting readers. */
static int kring_sendmsg( struct kiocb *iocb, struct socket *sock, struct msghdr *msg, size_t len )
{
	struct kring_sock *krs = kring_sk( sock->sk );
	struct kring_ringset *r = krs->ringset;
	wake_up_interruptible_all( &r->reader_waitqueue );
	return 0;
}

static void kring_sock_destruct( struct sock *sk )
{
	struct kring_sock *krs = kring_sk( sk );
	int i;

	if ( krs == 0 ) {
		printk("kring_sock_destruct: don't have krs\n" );
		return;
	}

	printk( "kring_sock_destruct mode: %d ring_id: %d reader_id: %d\n",
			krs->mode, krs->ring_id, krs->reader_id );

	switch ( krs->mode ) {
		case KRING_WRITE: {
			krs->ringset->ring[krs->ring_id].num_writers -= 1;
			break;
		}
		case KRING_READ: {

			/* One ring or all? */
			if ( krs->ring_id != KRING_RING_ID_ALL ) {
				struct kring_control *control = &krs->ringset->ring[krs->ring_id].control;

				krs->ringset->ring[krs->ring_id].reader[krs->reader_id].allocated = false;

				if ( control->reader[krs->reader_id].entered ) {
					kring_off_t prev = control->reader[krs->reader_id].rhead;
					kring_reader_release( krs->reader_id, &control[0], prev );
				}
			}
			else {
				for ( i = 0; i < krs->ringset->nrings; i++ ) {
					struct kring_control *control = &krs->ringset->ring[i].control;

					krs->ringset->ring[i].reader[krs->reader_id].allocated = false;

					if ( control->reader[krs->reader_id].entered ) {
						kring_off_t prev = control->reader[krs->reader_id].rhead;
						kring_reader_release( krs->reader_id, &control[0], prev );
					}
				}
			}
		}
	}
}

int kring_sock_create( struct net *net, struct socket *sock, int protocol, int kern )
{
	struct sock *sk;

//	/* Privs? */
//	if ( !capable( CAP_NET_ADMIN ) )
//		return -EPERM;

	if ( sock->type != SOCK_RAW )
		return -ESOCKTNOSUPPORT;

	if ( protocol != htons(ETH_P_ALL) )
		return -EPROTONOSUPPORT;

	sk = sk_alloc( net, PF_INET, GFP_KERNEL, &kring_proto );

	if ( sk == NULL )
		return -ENOMEM;

	sock->ops = &kring_ops;
	sock_init_data( sock, sk );

	sk->sk_family = KRING;
	sk->sk_destruct = kring_sock_destruct;

	return 0;
}

int kring_kopen( struct kring_kern *kring, const char *rsname, int ring_id, enum KRING_MODE mode )
{
	int reader_id = -1, writer_id = -1;

	struct kring_ringset *ringset = find_ring( rsname );
	if ( ringset == 0 )
		return -1;
	
	if ( ring_id < 0 || ring_id >= ringset->nrings )
		return -1;

	kring_copy_name( kring->name, rsname );
	kring->ringset = ringset;
	kring->ring_id = ring_id;

	if ( mode == KRING_WRITE ) {
		/* Find a writer ID. */
		writer_id = kring_allocate_writer_on_ring( &ringset->ring[ring_id] );
		if ( writer_id < 0 )
			return writer_id;
	}
	else if ( mode == KRING_READ ) {
		/* Find a reader ID. */
		if ( ring_id != KRING_RING_ID_ALL ) {
			/* Reader ID for ring specified. */
			reader_id = kring_allocate_reader_on_ring( &ringset->ring[ring_id] );
		}
		else {
			/* Find a reader ID that works for all rings. This can fail. */
			reader_id = kring_allocate_reader_all_rings( ringset );
		}

		if ( reader_id < 0 )
			return reader_id;

		/*res = */kring_prep_enter( &ringset->ring[ring_id].control, 0 );
		//if ( res < 0 ) {
		//	kring_func_error( KRING_ERR_ENTER, 0 );
		//	return -1;
		//}
	}

	/* Set up the user read/write struct for unified read/write operations between kernel and user space. */
	kring->user.socket = -1;
	kring->user.control = &ringset->ring[ring_id].control;
	kring->user.data = 0;
	kring->user.pd = ringset->ring[ring_id].pd;
	kring->user.mode = mode;
	kring->user.ring_id = ring_id;
	kring->user.reader_id = reader_id;
	kring->user.writer_id = writer_id;
	kring->user.nrings = ringset->nrings;

	return 0;
}

int kring_kclose( struct kring_kern *kring )
{
	kring->ringset->ring[kring->ring_id].num_writers -= 1;
	return 0;
}

static void kring_write_single( struct kring_kern *kring, int dir,
		const struct sk_buff *skb, int offset, int write, int len )
{
	struct kring_packet_header *h;
	void *pdata;

	/* Which ringset? */
	struct kring_ringset *r = kring->ringset;

	h = kring_write_FIRST( &kring->user );

	h->len = len;
	h->dir = (char) dir;

	pdata = (char*)(h + 1);
	skb_copy_bits( skb, offset, pdata, write );

	kring_write_SECOND( &kring->user );

	/* track the number of packets produced. Note we don't account for overflow. */
	__sync_add_and_fetch( &r->ring[kring->ring_id].control.head->produced, 1 );

	#if 0
	for ( id = 0; id < KRING_READERS; id++ ) {
		if ( r->ring[kring->ring_id].reader[id].allocated ) {
			unsigned long long diff =
					r->ring[kring->ring_id].control.writer->produced -
					r->ring[kring->ring_id].control.reader[id].units;
			// printk( "diff: %llu\n", diff );
			if ( diff > ( KRING_NPAGES / 2 ) ) {
				printk( "half full: ring_id: %d reader_id: %d\n", kring->ring_id, id );
			}
		}
	}
	#endif

	wake_up_interruptible_all( &r->reader_waitqueue );
}

void kring_kwrite( struct kring_kern *kring, int dir, const struct sk_buff *skb )
{
	int offset = 0, write, len;

	while ( true ) {
		/* Number of bytes left in the packet (maybe overflows) */
		len = skb->len - offset;
		if ( len <= 0 )
			break;

		/* What can we write this time? */
		write = len;
		if ( write > kring_packet_max_data() )
			write = kring_packet_max_data();

		kring_write_single( kring, dir, skb, offset, write, len );

		offset += write;
	}
}

int kring_kavail( struct kring_kern *kring )
{
	struct kring_ring *ring = &kring->ringset->ring[kring->ring_id];
	int reader_id = 0;

	return ring->num_writers > 0 &&
		( ring->control.reader[reader_id].rhead != ring->control.head->whead );
}

void kring_knext_plain( struct kring_kern *kring, struct kring_plain *plain )
{
	struct kring_plain_header *h;

	h = (struct kring_plain_header*) kring_next_generic( &kring->user );

	plain->len = h->len;
	plain->bytes = (unsigned char*)(h + 1);
}

static void *kring_alloc_shared_memory( int size )
{
	void *mem;
	size = PAGE_ALIGN(size);
	mem = vmalloc_user(size);
	memset( mem, 0, size );
	return mem;
}

static void kring_free_shared_memory( void *m )
{
	vfree( m );
}

static void kring_ring_alloc( struct kring_ring *r )
{
	int i;

	r->ctrl = kring_alloc_shared_memory( KRING_CTRL_SZ );

	r->control.head = r->ctrl + KRING_CTRL_OFF_HEAD;
	r->control.writer = r->ctrl + KRING_CTRL_OFF_WRITER;
	r->control.reader = r->ctrl + KRING_CTRL_OFF_READER;
	r->control.descriptor = r->ctrl + KRING_CTRL_OFF_DESC;

	r->num_writers = 0;
	r->num_readers = 0;

	r->pd = kmalloc( sizeof(struct kring_page_desc) * KRING_NPAGES, GFP_KERNEL );
	for ( i = 0; i < KRING_NPAGES; i++ ) {
		r->pd[i].p = alloc_page( GFP_KERNEL | __GFP_ZERO );
		if ( r->pd[i].p ) {
			r->pd[i].m = page_address(r->pd[i].p);
		}
		else {
			printk( "alloc_page for ring allocation failed\n" );
		}
	}

	r->control.head->whead = r->control.head->wresv = kring_one_back( 0 );

	r->control.head->write_mutex = 0;

	for ( i = 0; i < KRING_READERS; i++ )
		r->reader[i].allocated = false;

	init_waitqueue_head( &r->reader_waitqueue );
}

static void kring_ringset_alloc( struct kring_ringset *r, const char *name, long nrings )
{
	int i;

	printk( "allocating %ld rings\n", nrings );

	strncpy( r->name, name, KRING_NLEN );
	r->name[KRING_NLEN-1] = 0;

	r->nrings = nrings;

	r->ring = kmalloc( sizeof(struct kring_ring) * nrings, GFP_KERNEL );
	memset( r->ring, 0, sizeof(struct kring_ring) * nrings  );

	for ( i = 0; i < nrings; i++ )
		kring_ring_alloc( &r->ring[i] );

	init_waitqueue_head( &r->reader_waitqueue );
}

static void kring_ring_free( struct kring_ring *r )
{
	int i;
	for ( i = 0; i < KRING_NPAGES; i++ )
		__free_page( r->pd[i].p );

	kring_free_shared_memory( r->ctrl );
	kfree( r->pd );
}

static void kring_ringset_free( struct kring_ringset *r )
{
	int i;
	for ( i = 0; i < r->nrings; i++ )
		kring_ring_free( &r->ring[i] );
	kfree( r->ring );
}

static void kring_add_ringset( struct kring_ringset **phead, struct kring_ringset *set )
{
	if ( *phead == 0 )
		*phead = set;
	else {
		struct kring_ringset *tail = *phead;
		while ( tail->next != 0 )
			tail = tail->next;
		tail->next = set;
	}
	set->next = 0;
}

ssize_t kring_add_data_store( struct kring *obj, const char *name, long rings_per_set )
{
	struct kring_ringset *r;
	if ( rings_per_set < 1 || rings_per_set > KRING_MAX_RINGS_PER_SET )
		return -EINVAL;

	r = kmalloc( sizeof(struct kring_ringset), GFP_KERNEL );
	kring_ringset_alloc( r, name, rings_per_set );

	kring_add_ringset( &head_data, r );

	return 0;
}

ssize_t kctl_add_cmd_store( const char *name );

ssize_t kring_add_cmd_store( struct kring *obj, const char *name )
{
	return kctl_add_cmd_store( name );
}

ssize_t kring_del_store( struct kring *obj, const char *name  )
{
	return 0;
}

ssize_t kctl_add_data_store( const char *name, long rings_per_set )
{
	struct kctl_ringset *r;
	if ( rings_per_set < 1 || rings_per_set > KCTL_MAX_RINGS_PER_SET )
		return -EINVAL;

	r = kmalloc( sizeof(struct kctl_ringset), GFP_KERNEL );
	kctl_ringset_alloc( r, name, rings_per_set );

	kctl_add_ringset( &head_cmd, r );

	return 0;
}

ssize_t kctl_add_cmd_store( const char *name )
{
	struct kctl_ringset *r;

	r = kmalloc( sizeof(struct kctl_ringset), GFP_KERNEL );
	kctl_ringset_alloc( r, name, 1 );

	kctl_add_ringset( &head_cmd, r );

	return 0;
}

ssize_t kctl_del_store( const char *name  )
{
	return 0;
}


int kctl_init(void);
int kring_init(void)
{
	int rc;

	sock_register(&kring_family_ops);

	if ( (rc = proto_register(&kring_proto, 0) ) != 0 )
		return rc;

	return kctl_init();;
}

static void kring_free_ringsets( struct kring_ringset *head )
{
	struct kring_ringset *r = head;
	while ( r != 0 ) {
		kring_ringset_free( r );
		r = r->next;
	}
}

void kctl_exit(void);
void kring_exit(void)
{
	kctl_exit();

	sock_unregister( KRING );

	proto_unregister( &kring_proto );

	kring_free_ringsets( head_data );
	kctl_free_ringsets( head_cmd );
}


EXPORT_SYMBOL_GPL(kring_kopen);
EXPORT_SYMBOL_GPL(kring_kclose);
EXPORT_SYMBOL_GPL(kring_kwrite);
EXPORT_SYMBOL_GPL(kring_kavail);
EXPORT_SYMBOL_GPL(kring_knext_plain);
