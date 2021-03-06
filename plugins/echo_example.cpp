/* <x0/plugins/echo_example.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#include <x0/http/HttpPlugin.h>
#include <x0/http/HttpServer.h>
#include <x0/http/HttpRequest.h>
#include <x0/http/HttpHeader.h>
#include <x0/io/BufferSource.h>
#include <x0/strutils.h>
#include <x0/Types.h>

#define TRACE(msg...) DEBUG("echo: " msg)

class EchoHandler
{
private:
	x0::HttpRequest* request_;

public:
	EchoHandler(x0::HttpRequest* in) :
		request_(in)
	{
	}

	void run()
	{
		// set response status code
		request_->status = x0::HttpError::Ok;

		// set response header "Content-Length",
		// if request content were not encoded
		// and if we've received its request header "Content-Length"
		if (!request_->requestHeader("Content-Encoding"))
			if (x0::BufferRef value = request_->requestHeader("Content-Length"))
				request_->responseHeaders.overwrite("Content-Length", value.str());

		// try to read content (if available) and pass it on to our onContent handler,
		// or fall back to just write HELLO (if no request content body was sent).

		if (request_->contentAvailable()) {
			request_->setBodyCallback<EchoHandler, &EchoHandler::onContent>(this);
		} else {
			request_->write<x0::BufferSource>("I'm an HTTP echo-server, dude.\n");
			request_->finish();
			delete this;
		}
	}

private:
	// Handler, invoked on request content body chunks,
	// which we want to "echo" back to the client.
	//
	// NOTE, this can be invoked multiple times, depending on the input.
	void onContent(const x0::BufferRef& chunk)
	{
		TRACE("onContent('%s')", chunk.str().c_str());
		request_->write<x0::BufferSource>(std::move(chunk));
		request_->writeCallback<EchoHandler, &EchoHandler::contentWritten>(this);
	}

	// Handler, invoked when a content chunk has been fully written to the client
	// (or an error occurred).
	//
	// We will try to read another input chunk to echo back to the client,
	// or just finish the response if failed.
	void contentWritten()
	{
		// TODO (write-)error handling; pass errno to this fn, too.

		// is there more data available?
		if (!request_->contentAvailable()) {
			// could not read another input chunk, so finish processing this request.
			request_->finish();
			delete this;
		}
	}
};

/**
 * \ingroup plugins
 * \brief echo content generator plugin
 */
class EchoPlugin :
	public x0::HttpPlugin
{
public:
	EchoPlugin(x0::HttpServer& srv, const std::string& name) :
		x0::HttpPlugin(srv, name)
	{
		registerHandler<EchoPlugin, &EchoPlugin::handleRequest>("echo_example");
	}

	~EchoPlugin()
	{
	}

private:
	virtual bool handleRequest(x0::HttpRequest *in, const x0::FlowParams& args)
	{
		// create a handler serving this very request.
		(new EchoHandler(in))->run();

		// yes, we are handling this request
		return true;
	}
};

X0_EXPORT_PLUGIN_CLASS(EchoPlugin)
