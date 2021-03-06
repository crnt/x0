/* <SocketSink.h>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#ifndef sw_x0_io_SocketSink_h
#define sw_x0_io_SocketSink_h 1

#include <x0/io/Sink.h>
#include <x0/Socket.h>
#include <x0/Types.h>

namespace x0 {

//! \addtogroup io
//@{

/** file descriptor stream sink.
 */
class X0_API SocketSink :
	public Sink
{
public:
	explicit SocketSink(Socket *conn);

	Socket *socket() const;
	void setSocket(Socket *);

	virtual void accept(SinkVisitor& v);
	virtual ssize_t write(const void *buffer, size_t size);
	ssize_t write(int fd, off_t *offset, size_t nbytes);

protected:
	Socket *socket_;
};

//@}

// {{{ impl
inline Socket *SocketSink::socket() const
{
	return socket_;
}

inline void SocketSink::setSocket(Socket *value)
{
	socket_ = value;
}

inline ssize_t SocketSink::write(int fd, off_t *offset, size_t nbytes)
{
	return socket_->write(fd, offset, nbytes);
}
// }}}

} // namespace x0

#endif
