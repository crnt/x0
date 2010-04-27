/* <x0/connection.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/connection.hpp>
#include <x0/listener.hpp>
#include <x0/request.hpp>
#include <x0/response.hpp>
#include <x0/server.hpp>
#include <x0/types.hpp>
#include <x0/sysconfig.h>

#if defined(WITH_SSL)
#	include <gnutls/gnutls.h>
#	include <gnutls/extra.h>
#endif

#include <functional>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#if 0
#	define TRACE(msg...)
#else
#	define TRACE(msg...) DEBUG("connection: " msg)
#endif

namespace x0 {

connection::connection(x0::listener& lst) :
	message_processor(message_processor::REQUEST),
	secure(false),
	listener_(lst),
	server_(lst.server()),
//	socket_(), // initialized in constructor body
	remote_ip_(),
	remote_port_(0),
	buffer_(8192),
	next_offset_(0),
	request_(new request(*this)),
	response_(0),
	io_state_(INVALID),
	watcher_(server_.loop())
#if defined(WITH_CONNECTION_TIMEOUTS)
	, timer_(server_.loop())
#endif
#if !defined(NDEBUG)
	, ctime_(ev_now(server_.loop()))
#endif
{
	watcher_.set<connection, &connection::io>(this);

#if defined(WITH_CONNECTION_TIMEOUTS)
	timer_.set<connection, &connection::timeout>(this);
#endif

}

connection::~connection()
{
	delete request_;
	delete response_;
	request_ = 0;
	response_ = 0;

	TRACE("~connection(%p)", this);

	try
	{
		server_.connection_close(this); // we cannot pass a shared pointer here as use_count is already zero and it would just lead into an exception though
	}
	catch (...)
	{
		TRACE("~connection(%p): unexpected exception", this);
	}

#if defined(WITH_SSL)
	if (ssl_enabled())
		gnutls_deinit(ssl_session_);
#endif

	if (socket_ != -1)
	{
		::close(socket_);
#if !defined(NDEBUG)
		socket_ = -1;
#endif
	}
}

void connection::io(ev::io& w, int revents)
{
	TRACE("connection(%p).io(revents=0x%04X)", this, revents);

#if defined(WITH_CONNECTION_TIMEOUTS)
	timer_.stop();
#endif

	if (revents & ev::READ)
		handle_read();

	if (revents & ev::WRITE)
		handle_write();
}

#if defined(WITH_CONNECTION_TIMEOUTS)
void connection::timeout(ev::timer& watcher, int revents)
{
	TRACE("connection(%p): timed out", this);

	watcher_.stop();
//	ev_unloop(loop(), EVUNLOOP_ONE);

	delete this;
}
#endif

#if defined(WITH_SSL)
void connection::ssl_initialize()
{
	gnutls_init(&ssl_session_, GNUTLS_SERVER);
	gnutls_priority_set(ssl_session_, listener_.priority_cache_);
	gnutls_credentials_set(ssl_session_, GNUTLS_CRD_CERTIFICATE, listener_.x509_cred_);

	gnutls_certificate_server_set_request(ssl_session_, GNUTLS_CERT_REQUEST);
	gnutls_dh_set_prime_bits(ssl_session_, 1024);

	gnutls_session_enable_compatibility_mode(ssl_session_);

	gnutls_transport_set_ptr(ssl_session_, (gnutls_transport_ptr_t)handle());

	listener_.ssl_db().bind(ssl_session_);
}

bool connection::ssl_enabled() const
{
	return listener_.secure();
}
#endif

/** start first async operation for this connection.
 *
 * This is done by simply registering the underlying socket to the the I/O service
 * to watch for available input.
 * \see stop()
 * :
 */
void connection::start()
{
	socklen_t slen = sizeof(saddr_);
	memset(&saddr_, 0, slen);

	socket_ = ::accept(listener_.handle(), reinterpret_cast<sockaddr *>(&saddr_), &slen);

	if (socket_ < 0)
	{
		server_.log(severity::error, "Could not accept client socket: %s", strerror(errno));
		delete this;
		return;
	}

	TRACE("connection(%p).start() fd=%d", this, socket_);

	if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0)
		printf("could not set server socket into non-blocking mode: %s\n", strerror(errno));

#if defined(TCP_NODELAY)
	if (server_.tcp_nodelay())
	{
		int flag = 1;
		setsockopt(socket_, SOL_TCP, TCP_NODELAY, &flag, sizeof(flag));
	}
#endif

	server_.connection_open(this);

	if (socket_ < 0) // hook triggered delayed delete via connection::close()
	{
		delete this;
		return;
	}

#if defined(WITH_SSL)
	if (ssl_enabled())
	{
		handshaking_ = true;
		ssl_initialize();
		ssl_handshake();
		return;
	}
	else
		handshaking_ = false;
#endif

#if defined(TCP_DEFER_ACCEPT)
	// it is ensured, that we have data pending, so directly start reading
	handle_read();
#else
	// client connected, but we do not yet know if we have data pending
	start_read();
#endif
}

#if defined(WITH_SSL)
bool connection::ssl_handshake()
{
	int rv = gnutls_handshake(ssl_session_);
	if (rv == GNUTLS_E_SUCCESS)
	{
		// handshake either completed or failed
		handshaking_ = false;
		TRACE("SSL handshake time: %.4f", ev_now(server_.loop()) - ctime_);
		start_read();
		return true;
	}

	if (rv != GNUTLS_E_AGAIN && rv != GNUTLS_E_INTERRUPTED)
	{
		TRACE("SSL handshake failed (%d): %s", rv, gnutls_strerror(rv));

		delete this;
		return false;
	}

	TRACE("SSL handshake incomplete: (%d)", gnutls_record_get_direction(ssl_session_));
	switch (gnutls_record_get_direction(ssl_session_))
	{
		case 0: // read
			start_read();
			break;
		case 1: // write
			start_write();
			break;
		default:
			break;
	}
	return false;
}
#endif

inline bool url_decode(buffer_ref& url)
{
	std::size_t left = url.offset();
	std::size_t right = left + url.size();
	std::size_t i = left; // read pos
	std::size_t d = left; // write pos
	buffer& value = url.buffer();

	while (i != right)
	{
		if (value[i] == '%')
		{
			if (i + 3 <= right)
			{
				int ival;
				if (hex2int(value.begin() + i + 1, value.begin() + i + 3, ival))
				{
					value[d++] = static_cast<char>(ival);
					i += 3;
				}
				else
				{
					return false;
				}
			}
			else
			{
				return false;
			}
		}
		else if (value[i] == '+')
		{
			value[d++] = ' ';
			++i;
		}
		else if (d != i)
		{
			value[d++] = value[i++];
		}
		else
		{
			++d;
			++i;
		}
	}

	url = value.ref(left, d - left);
	return true;
}

void connection::message_begin(buffer_ref&& method, buffer_ref&& uri, int version_major, int version_minor)
{
	TRACE("message_begin('%s', '%s', HTTP/%d.%d)", method.str().c_str(), uri.str().c_str(), version_major, version_minor);

	request_->method = std::move(method);

	request_->uri = std::move(uri);
	url_decode(request_->uri);

	std::size_t n = request_->uri.find("?");
	if (n != std::string::npos)
	{
		request_->path = request_->uri.ref(0, n);
		request_->query = request_->uri.ref(n + 1);
	}
	else
	{
		request_->path = request_->uri;
	}

	request_->http_version_major = version_major;
	request_->http_version_minor = version_minor;
}

void connection::message_header(buffer_ref&& name, buffer_ref&& value)
{
	request_->headers.push_back(request_header(std::move(name), std::move(value)));
}

bool connection::message_header_done()
{
	TRACE("message_header_done()");
	response_ = new response(this);
	try
	{
		server_.handle_request(request_, response_);
	}
	catch (response::code_type ec)
	{
		TRACE("message_header_done: error code (%d) catched", ec);
		response_->status = ec;
		response_->finish();
	}
	catch (...)
	{
		TRACE("message_header_done: unhandled exception caught");
		response_->status = 500;
		response_->finish();
	}

	return true;
}

bool connection::message_content(buffer_ref&& chunk)
{
	TRACE("message_content()");

	if (request_->read_callback_)
		request_->read_callback_(std::move(chunk));

	return false;
}

bool connection::message_end()
{
	TRACE("message_end()");
	return false;
}

/** Resumes async operations.
 *
 * This method is being invoked on a keep-alive connection to parse further requests.
 * \see start()
 */
void connection::resume(bool finish)
{
	TRACE("connection(%p).resume(finish=%s)", this, finish ? "true" : "false");

	if (finish)
	{
		delete response_;
		response_ = 0;

		delete request_;
		request_ = 0;

		request_ = new request(*this);

		assert(state() == message_processor::MESSAGE_BEGIN);
	}

	if (next_offset_ && next_offset_ < buffer_.size()) // HTTP pipelining
	{
		TRACE("resume(): pipelined %ld bytes", buffer_.size() - next_offset_);
		process();
	}
	else
	{
		TRACE("resume(): start read");

		if (finish)
		{
			next_offset_ = 0;
			buffer_.clear();
			clear();
		}

		start_read();
	}
}

void connection::start_read()
{
	switch (io_state_)
	{
		case INVALID:
			TRACE("start_read(): start watching");
			io_state_ = READING;
			watcher_.set(socket_, ev::READ);
			watcher_.start();
			break;
		case READING:
			TRACE("start_read(): continue reading (fd=%d)", socket_);
			break;
		case WRITING:
			io_state_ = READING;
			TRACE("start_read(): continue reading (fd=%d) (was ev::WRITE)", socket_);
			watcher_.set(socket_, ev::READ);
			break;
	}

#if defined(WITH_CONNECTION_TIMEOUTS)
	if (server_.max_read_idle() > 0)
		timer_.start(server_.max_read_idle(), 0.0);
#endif
}

void connection::start_write()
{
	if (io_state_ != WRITING)
	{
		TRACE("start_write(): start watching");
		io_state_ = WRITING;
		watcher_.set(socket_, ev::WRITE);
	}
	else
		TRACE("start_write(): continue watching");

#if defined(WITH_CONNECTION_TIMEOUTS)
	if (server_.max_write_idle() > 0)
		timer_.start(server_.max_write_idle(), 0.0);
#endif
}

void connection::stop_write()
{
	TRACE("stop_write()");
	start_read();
}

void connection::handle_write()
{
	TRACE("connection(%p).handle_write()", this);

#if defined(WITH_SSL)
	if (handshaking_)
		return (void) ssl_handshake();

//	if (ssl_enabled())
//		rv = gnutls_write(ssl_session_, buffer_.capacity() - buffer_.size());
//	else if (write_some)
//		write_some(this);
#else
#endif

#if 0
	buffer write_buffer_;
	int write_offset_ = 0;

	int rv = ::write(socket_, write_buffer_.begin() + write_offset_, write_buffer_.size() - write_offset_);

	if (rv < 0)
		; // error
	else
	{
		write_offset_ += rv;
		start_write();
	}
#else
	if (write_some)
		write_some(this);
#endif

	if (socket_ < 0)
		delete this;
}

void connection::check_request_body()
{
	if (state() == message_processor::MESSAGE_BEGIN)
		return;

	TRACE("request body not (yet) fully consumed: state=%s", state_str());
}

/**
 * This method gets invoked when there is data in our connection ready to read.
 *
 * We assume, that we are in request-parsing state.
 */
void connection::handle_read()
{
	TRACE("connection(%p).handle_read()", this);

#if defined(WITH_SSL)
	if (handshaking_)
		return (void) ssl_handshake();
#endif

	int rv = buffer_.size();

#if defined(WITH_SSL)
	if (ssl_enabled())
		rv = gnutls_read(ssl_session_, buffer_.end(), buffer_.capacity() - buffer_.size());
	else
		rv = ::read(socket_, buffer_.end(), buffer_.capacity() - buffer_.size());
#else
	rv = ::read(socket_, buffer_.end(), buffer_.capacity() - buffer_.size());
#endif

	if (rv < 0) // error
	{
		if (errno == EAGAIN || errno == EINTR)
		{
			start_read();
			ev_unloop(server_.loop(), EVUNLOOP_ONE);
		}
		else
		{
			TRACE("connection::handle_read(): %s", strerror(errno));
			delete this;
		}
	}
	else if (rv == 0) // EOF
	{
		TRACE("connection::handle_read(): (EOF)");
		delete this;
	}
	else
	{
		TRACE("connection::handle_read(): read %d bytes", rv);

		buffer_.resize(buffer_.size() + rv);
		process();

		if (socket_ < 0)
			delete this;
	}
}

/** closes this connection, possibly deleting this object (or propagating delayed delete).
 */
void connection::close()
{
	TRACE("connection: close(): state=%d", io_state_);
	switch (io_state_)
	{
		case INVALID:
			// we've got invoked from within connection_open()-hook (within the connection::start()-call)
			/* fall through */
		case READING:
		case WRITING:
			::close(socket_);
			socket_ = -1;
			break;
			break;
	}
}

/** processes a (partial) request from buffer's given \p offset of \p count bytes.
 */
void connection::process()
{
	TRACE("process: next_offset=%ld, size=%ld", next_offset_, buffer_.size());

	std::error_code ec = message_processor::process(
			buffer_.ref(next_offset_, buffer_.size() - next_offset_),
			next_offset_);

	TRACE("process: ec=%s", ec.message().c_str());

	if (ec == http_message_error::partial)
	{
		start_read();
	}
	else if (ec != http_message_error::aborted)
	{
		// -> send stock response: BAD_REQUEST
		(response_ = new response(this, response::bad_request))->finish();
	}
}

std::string connection::remote_ip() const
{
	if (remote_ip_.empty())
	{
		char buf[128];

		if (inet_ntop(AF_INET6, &saddr_.sin6_addr, buf, sizeof(buf)))
			remote_ip_ = buf;
	}

	return remote_ip_;
}

int connection::remote_port() const
{
	if (!remote_port_)
	{
		remote_port_ = ntohs(saddr_.sin6_port);
	}

	return remote_port_;
}

std::string connection::local_ip() const
{
	return listener_.address();
}

int connection::local_port() const
{
	return listener_.port();
}

} // namespace x0
