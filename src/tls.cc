
#include <thread.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>

#define PEER_CN_NAME_LEN 256

FdDesc *Thread::prepSslClient( const char *remoteHost, int connFd )
{
	FdDesc *fdDesc = new FdDesc( FdDesc::Client );
	SelectFd *selectFd = new SelectFd( connFd, fdDesc, SelectFd::Connect, 0, 0, remoteHost );

	fdDesc->fd = selectFd;
	fdDesc->fd->remoteHost = remoteHost;

	return fdDesc;
}

SSL *Thread::startSslClient( SSL_CTX *clientCtx, const char *remoteHost, int connFd )
{
	makeNonBlocking( connFd );

	BIO *bio = BIO_new_fd( connFd, BIO_NOCLOSE );

	/* Create the SSL object and set it in the secure BIO. */
	SSL *ssl = SSL_new( clientCtx );
	SSL_set_mode( ssl, SSL_MODE_AUTO_RETRY );
	SSL_set_bio( ssl, bio, bio );
	SSL_set_tlsext_host_name( ssl, remoteHost );

	/* FIXME: this will get lost under current transfer scheme in tls proxy,
	 * but needed for a general implementation. */
	FdDesc *fdDesc = new FdDesc( FdDesc::Client );

	SelectFd *selectFd = new SelectFd( connFd, fdDesc, SelectFd::Connect, ssl, bio, strdup(remoteHost) );

	fdDesc->fd = selectFd;

	// selectFd->wantRead = true;
	selectFd->wantWrite = true;
	selectFdList.append( selectFd );

	return ssl;
}

void Thread::clientConnect( SelectFd *fd, uint8_t readyMask )
{
	int result = SSL_connect( fd->ssl );
	if ( result <= 0 ) {
		/* No connect yet. May need more data. */
		result = SSL_get_error( fd->ssl, result );

		fd->wantRead = false;
		fd->wantWrite = false;
		if ( result == SSL_ERROR_WANT_READ )
			fd->wantRead = true;
		else if ( result == SSL_ERROR_WANT_WRITE )
			fd->wantWrite = true;
		else {
			// FIXME: sslError( result );
		}
	}
	else {
		/* Check the verification result. */
		long verifyResult = SSL_get_verify_result( fd->ssl );
		if ( verifyResult != X509_V_OK ) {
			log_ERROR( "ssl peer failed verify: " << fd->remoteHost );
		}

		/* Check the cert chain. The chain length is automatically checked by
		 * OpenSSL when we set the verify depth in the CTX */

		/* Check the common name. */
		X509 *peer = SSL_get_peer_certificate( fd->ssl );
		char peer_CN[PEER_CN_NAME_LEN];
		X509_NAME_get_text_by_NID( X509_get_subject_name(peer),
				NID_commonName, peer_CN, PEER_CN_NAME_LEN );

		int cr = X509_check_host( peer, fd->remoteHost, strlen(fd->remoteHost), 0, 0 );
		if ( cr != 1 ) {
			log_ERROR( "ssl peer cn host mismatch: requested " <<
					fd->remoteHost << " but cert is for " << peer_CN );
		}

		/* Create a BIO for the ssl wrapper. */
		BIO *bio = BIO_new( BIO_f_ssl() );
		BIO_set_ssl( bio, fd->ssl, BIO_NOCLOSE );

		sslConnectSuccess( fd, fd->ssl, bio );
	}
}

FdDesc *Thread::startSslServer( SSL_CTX *defaultCtx, int fd )
{
	BIO *bio = BIO_new_fd( fd, BIO_NOCLOSE );

	/* Create the SSL object an set it in the secure BIO. */
	SSL *ssl = SSL_new( defaultCtx );
	SSL_set_mode( ssl, SSL_MODE_AUTO_RETRY );
	SSL_set_bio( ssl, bio, bio );

	FdDesc *fdDesc = new FdDesc( FdDesc::Server );
	SelectFd *selectFd = new SelectFd( fd, fdDesc, SelectFd::Accept, ssl, bio, 0 );

	fdDesc->fd = selectFd;

	selectFd->wantRead = true;
	// selectFd->wantWrite = true;

	selectFdList.append( selectFd );

	return fdDesc;
}

void Thread::dataRecv( SelectFd *fd, FdDesc *fdDesc, uint8_t readyMask )
{
//	log_debug( DBG_THREAD, "ready in data state " << 
//			( fdDesc->type == FdDesc::Client ? "(client)" : "(server)" ) );

	AnotherRead:
	{
		int nbytes = BIO_read( fdDesc->fd->bio, fdDesc->input, fdDesc->linelen );

		/* break when client closes the connection. */
		if ( nbytes <= 0 ) {
//			log_debug( DBG_THREAD, "bio read returned: " << nbytes );

			/* If the BIO is saying it we should retry later, go back into
			 * select. */
			if ( BIO_should_retry( fdDesc->fd->bio ) ) {
//				log_debug( DBG_THREAD, "bio should retry" );
				fd->wantRead = true; //BIO_should_read(fdDesc->bio);
				fd->wantWrite = BIO_should_write(fdDesc->fd->bio);
			}
			else {
//				log_debug( DBG_THREAD, "bio closed" );

				if ( BIO_shutdown_wr( fdDesc->other->fd->bio ) );
				//::close( fdDesc->other->fd->fd );
				::shutdown( fdDesc->other->fd->fd, SHUT_WR );
				fdDesc->other->fd->wantWrite = false;

				fdDesc->fd->state = SelectFd::Closed;

				fd->wantRead = false;
				fd->wantWrite = false;

				if ( fdDesc->other->fd->state == SelectFd::Closed ) {
					/* DONE */
					breakLoop();
				}
			}
		}
		else {
			if ( sslReadReady( fd, fdDesc, readyMask, nbytes ) )
				goto AnotherRead;
		}
	}
}

int Thread::write( FdDesc *fdDesc, uint8_t readyMask, char *data, int length )
{
	int written = BIO_write( fdDesc->fd->bio, data, length );

	if ( written <= 0 ) {
		/* If the BIO is saying it we should retry later, go back into select.
		 * */
		if ( BIO_should_retry( fdDesc->fd->bio ) ) {
			/* Write failure is retry-related. */
			fdDesc->fd->wantRead = BIO_should_read(fdDesc->fd->bio);
			fdDesc->fd->wantWrite = BIO_should_write(fdDesc->fd->bio);
			fdDesc->fd->abortRound = true;
			fdDesc->fd->state = SelectFd::WriteRetry;
		}
		else {
			/* Write failed for some non-retry reason. */
			log_ERROR( "SSL write failed for non-retry reason" );
		}
	}
	else {
		/* Wrote something. Write it all? */
		if ( written < length ) {
			fdDesc->fd->wantRead = false;
			fdDesc->fd->wantWrite = true;
			fdDesc->fd->abortRound = true;
			fdDesc->fd->state = SelectFd::WriteRetry;
		}
		else {
			/* Wrote it all. Ensure other half is back in the established state
			 * (maybe never left). */
			fdDesc->fd->state = SelectFd::Established;
			fdDesc->fd->wantRead = true;
			fdDesc->fd->wantWrite = false;
		}
	}

	return written;
}
