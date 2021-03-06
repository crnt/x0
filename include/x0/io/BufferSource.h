/* <BufferSource.h>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.ws/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#ifndef sw_x0_io_BufferSource_hpp
#define sw_x0_io_BufferSource_hpp 1

#include <x0/Buffer.h>
#include <x0/io/Source.h>
#include <memory>

namespace x0 {

//! \addtogroup io
//@{

/** buffer source.
 *
 * \see Buffer, Source, Sink
 */
class X0_API BufferSource :
	public Source
{
public:
	template<typename PodType, std::size_t N> explicit BufferSource(PodType (&value)[N]);
	explicit BufferSource(const x0::BufferRef& data);
	explicit BufferSource(x0::BufferRef&& data);
	explicit BufferSource(const Buffer& data);
	explicit BufferSource(Buffer&& data);

	std::size_t size() const;
	bool empty() const;

	const Buffer& buffer() const;

	virtual ssize_t sendto(Sink& sink);

	virtual const char* className() const;

private:
	Buffer buffer_;
	std::size_t pos_;
};

//@}

// {{{ inlines
template<typename PodType, std::size_t N>
inline BufferSource::BufferSource(PodType (&value)[N]) :
	buffer_(value, N - 1), pos_(0)
{
}

inline BufferSource::BufferSource(const x0::BufferRef& data) :
	buffer_(data), pos_(0)
{
}

inline BufferSource::BufferSource(x0::BufferRef&& data) :
	buffer_(std::move(data)), pos_(0)
{
}

inline BufferSource::BufferSource(const Buffer& data) :
	buffer_(data), pos_(0)
{
}

inline BufferSource::BufferSource(Buffer&& data) :
	buffer_(std::move(data)), pos_(0)
{
}

inline std::size_t BufferSource::size() const
{
	return buffer_.size();
}

inline bool BufferSource::empty() const
{
	return buffer_.empty();
}

inline const Buffer& BufferSource::buffer() const
{
	return buffer_;
}
// }}}

} // namespace x0

#endif
