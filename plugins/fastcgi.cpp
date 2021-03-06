/* <x0/plugins/fastcgi.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 *
 * --------------------------------------------------------------------------
 *
 * plugin type: content generator
 *
 * description:
 *     Produces a response based on the specified FastCGI backend.
 *     This backend is communicating  via TCP/IP.
 *     Plans to support named sockets exist, but you may
 *     jump in and contribute aswell.
 *
 * compliance:
 *     This plugin will support FastCGI protocol, however, it will not
 *     try to multiplex multiple requests over a single FastCGI transport
 *     connection, that is, a single TCP socket.
 *     Instead, it will open a new transport connection for each parallel
 *     request, which makes things alot easier.
 *
 *     The FastCGI application will also be properly notified on
 *     early client aborts by either EndRecord packet or by a closed
 *     transport connection, both events denote the fact, that the client
 *     somehow is or gets disconnected to or by the server.
 *
 * setup API:
 *     none
 *
 * request processing API:
 *     handler fastcgi(string host_and_port);                 # e.g. ("127.0.0.1:3000")
 *     __todo handler fastcgi(ip host, int port)              # e.g. (127.0.0.1, 3000)
 *     __todo handler fastcgi(sequence of [ip host, int port])# e.g. (1.2.3.4, 988], [1.2.3.5, 988])
 *
 * todo:
 *     - error handling, including:
 *       - XXX early http client abort (raises EndRequestRecord-submission to application)
 *       - log stream parse errors,
 *       - transport level errors (connect/read/write errors)
 *       - timeouts
 */

#include "fastcgi_protocol.h"

#include <x0/http/HttpPlugin.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/http/HttpMessageProcessor.h>
#include <x0/io/BufferSource.h>
#include <x0/SocketSpec.h>
#include <x0/Logging.h>
#include <x0/strutils.h>
#include <x0/Process.h>
#include <x0/Buffer.h>
#include <x0/Types.h>
#include <x0/gai_error.h>
#include <x0/StackTrace.h>
#include <x0/sysconfig.h>

#include <system_error>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <deque>
#include <cctype>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#if !defined(NDEBUG)
#	define TRACE(msg...) (this->debug(msg))
#else
#	define TRACE(msg...) /*!*/
#endif

class CgiContext;
class CgiTransport;

inline std::string chomp(const std::string& value) // {{{
{
	if (value.size() && value[value.size() - 1] == '\n')
		return value.substr(0, value.size() - 1);
	else
		return value;
} // }}}

class CgiTransport : // {{{
	public x0::HttpMessageProcessor
#ifndef NDEBUG
	, public x0::Logging
#endif
{
	class ParamReader : public FastCgi::CgiParamStreamReader //{{{
	{
	private:
		CgiTransport *tx_;

	public:
		explicit ParamReader(CgiTransport *tx) : tx_(tx) {}

		virtual void onParam(const char *nameBuf, size_t nameLen, const char *valueBuf, size_t valueLen)
		{
			std::string name(nameBuf, nameLen);
			std::string value(valueBuf, valueLen);

			tx_->onParam(name, value);
		}
	}; //}}}
public:
	int refCount_;
	CgiContext *context_;

	uint16_t id_;
	std::string backendName_;
	x0::Socket* backend_;

	x0::Buffer readBuffer_;
	size_t readOffset_;
	x0::Buffer writeBuffer_;
	size_t writeOffset_;
	bool flushPending_;

	bool configured_;

	// aka CgiRequest
	x0::HttpRequest *request_;
	FastCgi::CgiParamStreamWriter paramWriter_;

public:
	explicit CgiTransport(CgiContext *cx);
	~CgiTransport();

	void ref();
	void unref();

	void bind(x0::HttpRequest *in, uint16_t id, x0::Socket* backend);
	void close();

	// server-to-application
	void abortRequest();

	// application-to-server
	void onStdOut(const x0::BufferRef& chunk);
	void onStdErr(const x0::BufferRef& chunk);
	void onEndRequest(int appStatus, FastCgi::ProtocolStatus protocolStatus);

	CgiContext& context() const { return *context_; }

public:
	template<typename T, typename... Args> void write(Args&&... args);
	void write(FastCgi::Type type, int requestId, x0::Buffer&& content);
	void write(FastCgi::Type type, int requestId, const char *buf, size_t len);
	void write(FastCgi::Record *record);
	void flush();

private:
	void processRequestBody(const x0::BufferRef& chunk);

	virtual bool onMessageHeader(const x0::BufferRef& name, const x0::BufferRef& value);
	virtual bool onMessageContent(const x0::BufferRef& content);

	void onWriteComplete();
	static void onClientAbort(void *p);

	void onConnectComplete(x0::Socket* s, int revents);
	void io(x0::Socket* s, int revents);

	inline bool processRecord(const FastCgi::Record *record);
	inline void onParam(const std::string& name, const std::string& value);
}; // }}}

class CgiContext //{{{
#ifndef NDEBUG
	: public x0::Logging
#endif
{
public:
	static uint16_t nextID_;

	x0::HttpServer& server_;
	x0::SocketSpec spec_;

public:
	CgiContext(x0::HttpServer& server);
	~CgiContext();

	x0::HttpServer& server() const { return server_; }
	void setup(const x0::SocketSpec& spec);

	void handleRequest(x0::HttpRequest *in);

	void release(CgiTransport *transport);
};
// }}}

// {{{ CgiTransport impl
CgiTransport::CgiTransport(CgiContext *cx) :
	HttpMessageProcessor(x0::HttpMessageProcessor::MESSAGE),
	refCount_(1),
	context_(cx),
	id_(1),
	backend_(nullptr),
	readBuffer_(),
	readOffset_(0),
	writeBuffer_(),
	writeOffset_(0),
	flushPending_(false),
	configured_(false),

	request_(nullptr),
	paramWriter_()
{
#ifndef NDEBUG
	static std::atomic<int> mi(0);
	setLoggingPrefix("CgiTransport/%d", ++mi);
#endif
	TRACE("create");

	// stream management record: GetValues
#if 0
	FastCgi::CgiParamStreamWriter mr;
	mr.encode("FCGI_MPXS_CONNS", "");
	mr.encode("FCGI_MAX_CONNS", "");
	mr.encode("FCGI_MAX_REQS", "");
	write(FastCgi::Type::GetValues, 0, mr.output());
#endif
}

CgiTransport::~CgiTransport()
{
	TRACE("destroy");

	if (backend_) {
		if (backend_->isOpen())
			backend_->close();

		delete backend_;
	}

	if (request_) {
		if (request_->status == x0::HttpError::Undefined) {
			request_->status = x0::HttpError::ServiceUnavailable;
		}

		request_->finish();
	}
}

void CgiTransport::close()
{
	if (backend_->isOpen()) {
		backend_->close();
		unref();
	}
}

void CgiTransport::ref()
{
	++refCount_;
}

void CgiTransport::unref()
{
	assert(refCount_ > 0);

	--refCount_;

	if (refCount_ == 0) {
		context_->release(this);
	}
}

void CgiTransport::bind(x0::HttpRequest *in, uint16_t id, x0::Socket* backend)
{
	// sanity checks
	assert(request_ == nullptr);
	assert(backend_ == nullptr);

	// initialize object
	id_ = id;
	backend_ = backend;
	request_ = in;
	request_->setAbortHandler(&CgiTransport::onClientAbort, this);

	// initialize stream
	write<FastCgi::BeginRequestRecord>(FastCgi::Role::Responder, id_, true);

	paramWriter_.encode("SERVER_SOFTWARE", PACKAGE_NAME "/" PACKAGE_VERSION);
	paramWriter_.encode("SERVER_NAME", request_->requestHeader("Host"));
	paramWriter_.encode("GATEWAY_INTERFACE", "CGI/1.1");

	paramWriter_.encode("SERVER_PROTOCOL", "1.1");
	paramWriter_.encode("SERVER_ADDR", request_->connection.localIP());
	paramWriter_.encode("SERVER_PORT", boost::lexical_cast<std::string>(request_->connection.localPort()));// TODO this should to be itoa'd only ONCE

	paramWriter_.encode("REQUEST_METHOD", request_->method);
	paramWriter_.encode("REDIRECT_STATUS", "200"); // for PHP configured with --force-redirect (Gentoo/Linux e.g.)

	request_->updatePathInfo(); // should we invoke this explicitely? I'd vote for no... however.

	paramWriter_.encode("SCRIPT_NAME", request_->path);
	paramWriter_.encode("PATH_INFO", request_->pathinfo);

	if (!request_->pathinfo.empty())
		paramWriter_.encode("PATH_TRANSLATED", request_->documentRoot, request_->pathinfo);

	paramWriter_.encode("QUERY_STRING", request_->query);			// unparsed uri
	paramWriter_.encode("REQUEST_URI", request_->uri);

	//paramWriter_.encode("REMOTE_HOST", "");  // optional
	paramWriter_.encode("REMOTE_ADDR", request_->connection.remoteIP());
	paramWriter_.encode("REMOTE_PORT", boost::lexical_cast<std::string>(request_->connection.remotePort()));

	//paramWriter_.encode("AUTH_TYPE", ""); // TODO
	//paramWriter_.encode("REMOTE_USER", "");
	//paramWriter_.encode("REMOTE_IDENT", "");

	if (request_->contentAvailable()) {
		paramWriter_.encode("CONTENT_TYPE", request_->requestHeader("Content-Type"));
		paramWriter_.encode("CONTENT_LENGTH", request_->requestHeader("Content-Length"));

		request_->setBodyCallback<CgiTransport, &CgiTransport::processRequestBody>(this);
	}

#if defined(WITH_SSL)
	if (request_->connection.isSecure())
		paramWriter_.encode("HTTPS", "on");
#endif

	// HTTP request headers
	for (auto& i: request_->requestHeaders) {
		std::string key;
		key.reserve(5 + i.name.size());
		key += "HTTP_";

		for (auto p = i.name.begin(), q = i.name.end(); p != q; ++p)
			key += std::isalnum(*p) ? std::toupper(*p) : '_';

		paramWriter_.encode(key, i.value);
	}
	paramWriter_.encode("DOCUMENT_ROOT", request_->documentRoot);
	paramWriter_.encode("SCRIPT_FILENAME", request_->fileinfo->path());

	write(FastCgi::Type::Params, id_, paramWriter_.output());
	write(FastCgi::Type::Params, id_, "", 0); // EOS

	// setup I/O callback
	if (backend_->state() == x0::Socket::Connecting)
		backend_->setReadyCallback<CgiTransport, &CgiTransport::onConnectComplete>(this);
	else
		backend_->setReadyCallback<CgiTransport, &CgiTransport::io>(this);

	// flush out
	flush();
}

template<typename T, typename... Args>
inline void CgiTransport::write(Args&&... args)
{
	T record(args...);
	write(&record);
}

inline void CgiTransport::write(FastCgi::Type type, int requestId, x0::Buffer&& content)
{
	write(type, requestId, content.data(), content.size());
}

void CgiTransport::write(FastCgi::Type type, int requestId, const char *buf, size_t len)
{
	const size_t chunkSizeCap = 0xFFFF;
	static const char padding[8] = { 0 };

	if (len == 0) {
		FastCgi::Record record(type, requestId, 0, 0);
		TRACE("CgiTransport.write(type=%s, rid=%d, size=0)", record.type_str(), requestId);
		writeBuffer_.push_back(record.data(), sizeof(record));
		return;
	}

	for (size_t offset = 0; offset < len; ) {
		size_t clen = std::min(offset + chunkSizeCap, len) - offset;
		size_t plen = clen % sizeof(padding)
					? sizeof(padding) - clen % sizeof(padding)
					: 0;

		FastCgi::Record record(type, requestId, clen, plen);
		writeBuffer_.push_back(record.data(), sizeof(record));
		writeBuffer_.push_back(buf + offset, clen);
		writeBuffer_.push_back(padding, plen);

		TRACE("CgiTransport.write(type=%s, rid=%d, offset=%ld, size=%ld, plen=%ld)",
				record.type_str(), requestId, offset, clen, plen);

		offset += clen;
	}
}

void CgiTransport::write(FastCgi::Record *record)
{
	TRACE("CgiTransport.write(type=%s, rid=%d, size=%d, pad=%d)",
			record->type_str(), record->requestId(), record->size(), record->paddingLength());

	writeBuffer_.push_back(record->data(), record->size());
}

void CgiTransport::flush()
{
	if (backend_->state() == x0::Socket::Operational) {
		TRACE("flush()");
		backend_->setMode(x0::Socket::ReadWrite);
	} else {
		TRACE("flush() -> pending");
		flushPending_ = true;
	}
}

/** invoked (by open() or asynchronousely by io()) to complete the connection establishment.
 * \retval true connection establishment completed
 * \retval false finishing connect() failed. object is also invalidated.
 */
void CgiTransport::onConnectComplete(x0::Socket* s, int revents)
{
	if (s->isClosed()) {
		TRACE("onConnectComplete() connect() failed");
		request_->status = x0::HttpError::ServiceUnavailable;
		unref();
	} else if (writeBuffer_.size() > writeOffset_ && flushPending_) {
		TRACE("onConnectComplete() flush pending");
		flushPending_ = false;
		backend_->setReadyCallback<CgiTransport, &CgiTransport::io>(this);
		backend_->setMode(x0::Socket::ReadWrite);
	} else {
		TRACE("onConnectComplete()");
		backend_->setReadyCallback<CgiTransport, &CgiTransport::io>(this);
		backend_->setMode(x0::Socket::Read);
	}
}

void CgiTransport::io(x0::Socket* s, int revents)
{
	TRACE("CgiTransport::io(0x%04x)", revents);
	ref();

	if (revents & x0::Socket::Read) {
		TRACE("CgiTransport::io(): reading ...");
		// read as much as possible
		for (;;) {
			size_t remaining = readBuffer_.capacity() - readBuffer_.size();
			if (remaining < 1024) {
				readBuffer_.reserve(readBuffer_.capacity() + 4*4096);
				remaining = readBuffer_.capacity() - readBuffer_.size();
			}

			int rv = backend_->read(readBuffer_);

			if (rv == 0) {
				TRACE("fastcgi: connection to backend lost.");
				goto app_err;
			}

			if (rv < 0) {
				if (errno != EINTR && errno != EAGAIN) { // TODO handle EWOULDBLOCK
					context().server().log(x0::Severity::error,
						"fastcgi: read from backend %s failed: %s",
						backendName_.c_str(), strerror(errno));
					goto app_err;
				}

				break;
			}
		}

		// process fully received records
		TRACE("CgiTransport::io(): processing ...");
		while (readOffset_ + sizeof(FastCgi::Record) < readBuffer_.size()) {
			const FastCgi::Record *record =
				reinterpret_cast<const FastCgi::Record *>(readBuffer_.data() + readOffset_);

			// payload fully available?
			if (readBuffer_.size() - readOffset_ < record->size())
				break;

			readOffset_ += record->size();

			if (!processRecord(record))
				goto done;
		}
	}

	if (revents & x0::Socket::Write) {
		TRACE("io(): writing to backend ...");
		ssize_t rv = backend_->write(writeBuffer_.ref(writeOffset_));
		TRACE("io(): write returned -> %ld ...", rv);

		if (rv < 0) {
			if (errno != EINTR && errno != EAGAIN) {
				context().server().log(x0::Severity::error,
					"fastcgi: write to backend %s failed: %s", backendName_.c_str(), strerror(errno));
				goto app_err;
			}

			goto done;
		}

		writeOffset_ += rv;

		// if set watcher back to EV_READ if the write-buffer has been fully written (to catch connection close events)
		if (writeOffset_ == writeBuffer_.size()) {
			TRACE("CgiTransport::io(): write buffer fully written to socket (%ld)", writeOffset_);
			backend_->setMode(x0::Socket::Read);
			writeBuffer_.clear();
			writeOffset_ = 0;
		}
	}
	goto done;

app_err:
	close();

done:
	unref();
}

bool CgiTransport::processRecord(const FastCgi::Record *record)
{
	TRACE("processRecord(type=%s (%d), rid=%d, contentLength=%d, paddingLength=%d)",
		record->type_str(), record->type(), record->requestId(),
		record->contentLength(), record->paddingLength());

	bool proceedHint = true;

	switch (record->type()) {
		case FastCgi::Type::GetValuesResult:
			ParamReader(this).processParams(record->content(), record->contentLength());
			configured_ = true; // should be set *only* at EOS of GetValuesResult? we currently guess, that there'll be only *one* packet
			break;
		case FastCgi::Type::StdOut:
			onStdOut(readBuffer_.ref(record->content() - readBuffer_.data(), record->contentLength()));
			break;
		case FastCgi::Type::StdErr:
			onStdErr(readBuffer_.ref(record->content() - readBuffer_.data(), record->contentLength()));
			break;
		case FastCgi::Type::EndRequest:
			onEndRequest(
				static_cast<const FastCgi::EndRequestRecord *>(record)->appStatus(),
				static_cast<const FastCgi::EndRequestRecord *>(record)->protocolStatus()
			);
			proceedHint = false;
			break;
		case FastCgi::Type::UnknownType:
		default:
			context().server().log(x0::Severity::error,
				"fastcgi: unknown transport record received from backend %s. type:%d, payload-size:%ld",
				backendName_.c_str(), record->type(), record->contentLength());
#if 1
			x0::Buffer::dump(record, sizeof(record), "fcgi packet header");
			x0::Buffer::dump(record->content(), std::min(record->contentLength() + record->paddingLength(), 512), "fcgi packet payload");
#endif
			break;
	}
	return proceedHint;
}

void CgiTransport::onParam(const std::string& name, const std::string& value)
{
	TRACE("onParam(%s, %s)", name.c_str(), value.c_str());
}

void CgiTransport::abortRequest()
{
	// TODO: install deadline-timer to actually close the connection if not done by the backend.
	if (backend_->isOpen()) {
		write<FastCgi::AbortRequestRecord>(id_);
		flush();
	} else {
		close();
	}
}

void CgiTransport::onStdOut(const x0::BufferRef& chunk)
{
	TRACE("CgiTransport.onStdOut: id=%d, chunk.size=%ld state=%s", id_, chunk.size(), state_str());
	process(chunk);
}

void CgiTransport::onStdErr(const x0::BufferRef& chunk)
{
	TRACE("CgiTransport.stderr(id:%d): %s", id_, chomp(chunk.str()).c_str());

	if (!request_)
		return;

	request_->log(x0::Severity::error, "fastcgi: %s", chomp(chunk.str()).c_str());
}

void CgiTransport::onEndRequest(int appStatus, FastCgi::ProtocolStatus protocolStatus)
{
	TRACE("CgiTransport.onEndRequest(appStatus=%d, protocolStatus=%d)", appStatus, (int)protocolStatus);
	close();
}

void CgiTransport::processRequestBody(const x0::BufferRef& chunk)
{
	TRACE("CgiTransport.processRequestBody(chunkLen=%ld, (r)contentLen=%ld)", chunk.size(),
			request_->connection.contentLength());

	// if chunk.size() is 0, this also marks the fcgi stdin stream's end. so just pass it.
	write(FastCgi::Type::StdIn, id_, chunk.data(), chunk.size());

	flush();
}

bool CgiTransport::onMessageHeader(const x0::BufferRef& name, const x0::BufferRef& value)
{
	TRACE("onResponseHeader(name:%s, value:%s)", name.str().c_str(), value.str().c_str());

	if (x0::iequals(name, "Status")) {
		int status = value.ref(0, value.find(' ')).toInt();
		request_->status = static_cast<x0::HttpError>(status);
	} else {
		if (name == "Location")
			request_->status = x0::HttpError::MovedTemporarily;

		request_->responseHeaders.push_back(name.str(), value.str());
	}

	return true;
}

bool CgiTransport::onMessageContent(const x0::BufferRef& content)
{
	TRACE("CgiTransport.messageContent(len:%ld)", content.size());

	request_->write<x0::BufferSource>(content);

	if (request_->connection.isOutputPending()) {
		backend_->setMode(x0::Socket::None);
		ref(); // will be unref'd in completion-handler, onWriteComplete().
		request_->writeCallback<CgiTransport, &CgiTransport::onWriteComplete>(this);
	}

	return false;
}

/** \brief write-completion hook, invoked when a content chunk is written to the HTTP client.
 */
void CgiTransport::onWriteComplete()
{
#if 0
	TRACE("CgiTransport.onWriteComplete() bufferSize: %ld", writeBuffer_.size());

	if (writeBuffer_.size() != 0) {
		TRACE("onWriteComplete: queued:%ld", writeBuffer_.size());

		request_->write<x0::BufferSource>(std::move(writeBuffer_));

		if (request_->connection.isOutputPending()) {
			TRACE("onWriteComplete: output pending. enqueue callback");
			request_->writeCallback<CgiTransport, &CgiTransport::onWriteComplete>(this);
			return;
		}
	}
#endif

	TRACE("onWriteComplete: output flushed. resume watching on app I/O (read)");
	backend_->setMode(x0::Socket::Read);
	unref(); // unref the ref(), invoked in messageContent()
}

/**
 * @brief invoked when remote client connected before the response has been fully transmitted.
 *
 * @param p `this pointer` to CgiTransport object
 */
void CgiTransport::onClientAbort(void *p)
{
	CgiTransport* self = reinterpret_cast<CgiTransport*>(p);

	// notify fcgi app about client abort
	self->abortRequest();
}
// }}}

// {{{ CgiContext impl
uint16_t CgiContext::nextID_ = 0;

CgiContext::CgiContext(x0::HttpServer& server) :
	server_(server),
	spec_()
{
}

CgiContext::~CgiContext()
{
}

void CgiContext::setup(const x0::SocketSpec& spec)
{
#ifndef NDEBUG
	setLoggingPrefix("CgiContext(%s)", spec.str().c_str());
#endif
	spec_ = spec;
}

void CgiContext::handleRequest(x0::HttpRequest *in)
{
	TRACE("CgiContext.handleRequest()");

	x0::Socket* backend = new x0::Socket(in->connection.worker().loop());
	backend->open(spec_, O_NONBLOCK | O_CLOEXEC);

	if (backend->isOpen()) {
		CgiTransport* transport = new CgiTransport(this);

		if (++nextID_ == 0)
			++nextID_;

		transport->bind(in, nextID_, backend);
	} else {
		// log(Severity::Error, "connection to backend failed: %s", backend->errorString());
		delete backend;
		in->status = x0::HttpError::ServiceUnavailable;
		in->finish();
	}
}

/**
 * \brief enqueues this transport connection ready for serving the next request.
 * \param transport the transport connection object
 */
void CgiContext::release(CgiTransport *transport)
{
	TRACE("CgiContext.release()");
	delete transport;
}
//}}}

// {{{ FastCgiPlugin
/**
 * \ingroup plugins
 * \brief serves static files from server's local filesystem to client.
 */
class FastCgiPlugin :
	public x0::HttpPlugin
{
public:
	FastCgiPlugin(x0::HttpServer& srv, const std::string& name) :
		x0::HttpPlugin(srv, name)
	{
		registerHandler<FastCgiPlugin, &FastCgiPlugin::handleRequest>("fastcgi");
	}

	~FastCgiPlugin()
	{
		for (auto i: contexts_)
			delete i.second;
	}

	bool handleRequest(x0::HttpRequest *in, const x0::FlowParams& args)
	{
		x0::SocketSpec spec;
		spec << args;

		if (!spec.isValid() || spec.backlog >= 0) {
			in->log(x0::Severity::error, "Invalid socket spec passed.");
			return false;
		}

		CgiContext *cx = acquireContext(spec);
		if (!cx)
			return false;

		cx->handleRequest(in);
		return true;
	}

	CgiContext *acquireContext(const x0::SocketSpec& spec)
	{
#if 0
		auto i = contexts_.find(app);
		if (i != contexts_.end()) {
			//TRACE("acquireContext('%s') available.", app.c_str());
			return i->second;
		}
#endif
		CgiContext *cx = new CgiContext(server());
		cx->setup(spec);
		//contexts_[app] = cx;
		//TRACE("acquireContext('%s') spawned (%ld).", app.c_str(), contexts_.size());
		return cx;
	}

private:
	std::unordered_map<std::string, CgiContext *> contexts_;
}; // }}}

X0_EXPORT_PLUGIN_CLASS(FastCgiPlugin)
