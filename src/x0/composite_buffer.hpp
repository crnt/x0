#ifndef x0_composite_buffer_hpp
#define x0_composite_buffer_hpp

#include <string>
#include <sys/types.h>
#include <sys/sendfile.h>	// sendfile()
#include <sys/socket.h>		// sendto()
#include <sys/stat.h>		// stat()
#include <unistd.h>			// close(), write(), sysconf()

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>

namespace x0 {

/**
 * Class for constructing and sending up to complex composite buffers from various sources.
 *
 * A composite buffer - once fully created - is meant to be sent only <b>once</b>.
 * This class shall support asynchronous I/O file.
 *
 * <code>
 * 	composite_buffer cb;
 *
 * 	cb.push_back("Hello, World");
 *  cb.push_back('\n');
 *
 * 	struct stat st;
 * 	if (stat("somefile.txt", &st) != -1)
 * 	{
 * 		int fd = open("somefile.txt", O_RDONLY);
 * 		if (fd != -1)
 * 		{
 * 			cb.push_back(fd, 0, st.st_size, true);
 * 		}
 * 	}
 *
 * 	cb.async_write(socket, completion_handler);
 *
 * 	void completion_handler(boost::system::error_code ec, std::size_t bytes_transferred)
 * 	{
 * 		if (ec)
 * 		{
 * 			throw std::runtime_error(strerror(errno));
 * 		}
 * 	}
 * </code>
 */
class composite_buffer
{
public:
	/** chunk base class.
	 * \see string_chunk, iovec_chunk, fd_chunk
	 */
	struct chunk // {{{
	{
		enum chunk_type { cstring, ciov, cfd } type;
		std::size_t size;
		chunk *next;

		explicit chunk(chunk_type ct, std::size_t sz) :
			type(ct), size(sz), next(0)
		{
		}

		virtual ~chunk()
		{
			delete next;
		}

		/** submits chunk data to given socket.
		 *
		 * \param socket the socket to write to.
		 */
		ssize_t write(boost::asio::ip::tcp::socket& socket)
		{
			ssize_t rv, total = 0;

			while (size)
			{
				if ((rv = write_some(socket)) != -1)
				{
					total += rv;
				}
				else
				{
					return rv;
				}
			}

			return total;
		}

		/** initiates chunk data submission, without blocking I/O.
		 *
		 * \param socket the socket to write to.
		 */
		virtual ssize_t write_some(boost::asio::ip::tcp::socket& socket) = 0;
	}; // }}}

	class iterator // {{{
	{
	private:
		const chunk *current;

	public:
		iterator(const chunk *c) :
			current(c)
		{
		}

		iterator& operator++()
		{
			if (current)
			{
				current = current->next;
			}

			return *this;
		}

		const chunk *operator->() const
		{
			return current;
		}

		const chunk& operator*() const
		{
			return *current;
		}

		friend bool operator!=(const iterator& a, const iterator& b)
		{
			return a.current != b.current;
		}
	}; // }}}

private:
	// {{{ chunk implementations
	/** string chunk. */
	struct string_chunk :
		public chunk
	{
		std::string value;
		off_t offset;

		string_chunk(const std::string _value) :
			chunk(cstring, _value.size()), value(_value), offset(0)
		{
		}

		void push_back(const std::string _value)
		{
			value += _value;
			size += _value.size();
		}

		virtual ssize_t write_some(boost::asio::ip::tcp::socket& socket)
		{
			ssize_t rv = ::sendto(socket.native(), value.data() + offset, size, MSG_NOSIGNAL, NULL, 0);

			if (rv != -1)
			{
				offset += rv;
				size -= rv;
			}

			return rv;
		}
	};

	struct iovec_chunk :
		public chunk
	{
		std::vector<iovec> vec;
		std::size_t veclimit;

		iovec_chunk() :
			chunk(ciov, 0), vec(), veclimit(sysconf(_SC_IOV_MAX))
		{
		}

		void push_back(const void *p, std::size_t n)
		{
			iovec iov;

			iov.iov_base = const_cast<void *>(p);
			iov.iov_len = n;

			vec.push_back(iov);
			size += n;
		}

		virtual ssize_t write_some(boost::asio::ip::tcp::socket& socket)
		{
			ssize_t rv = ::writev(socket.native(), &vec[0], vec.size());

			if (rv != -1)
			{
				size -= rv;
			}
			// TODO error handling if (rv != size), that is, not everything written on O_NONBLOCK
			// (does this even happen?)

			return rv;
		}
	};

	/** file chunk. */
	struct fd_chunk :
		public chunk
	{
		int fd;
		off_t offset;
		bool close;

		fd_chunk(int _fd, off_t _offset, size_t _size, bool _close) :
			chunk(cfd, _size), fd(_fd), offset(_offset), close(_close)
		{
		}

		~fd_chunk()
		{
			if (close)
			{
				::close(fd);
			}
		}

		virtual ssize_t write_some(boost::asio::ip::tcp::socket& socket)
		{
			if (size)
			{
				ssize_t rv = ::sendfile(socket.native(), fd, &offset, size);

				if (rv != -1)
				{
					size -= rv;
				}
            
				return rv;
			}
			else
			{
				return 0;
			}
		}
	};
	// }}}

	// {{{ class write_handler
	template<class CompletionHandler>
	class write_handler
	{
	private:
		composite_buffer& buffer_;
		boost::asio::ip::tcp::socket& socket_;
		CompletionHandler handler_;
		std::size_t total_bytes_transferred_;

	public:
		write_handler(composite_buffer& buffer, boost::asio::ip::tcp::socket& socket, const CompletionHandler& handler)
		  : buffer_(buffer), socket_(socket), handler_(handler)
		{
		}

		void operator()(const boost::system::error_code& ec, std::size_t bytes_transferred)
		{
			total_bytes_transferred_ += bytes_transferred;

			if (!ec && !buffer_.empty())
			{
				buffer_.async_write_some(socket_, *this);
			}
			else
			{
				handler_(ec, total_bytes_transferred_);
			}
		}
	};
	// }}}

	/**
	 * write some data into the screen.
	 *
	 * \param socket the socket to write to.
	 * \param handler the completion handler to invoke once current data has been sent 
	 *                and the socket is ready for new data to be transmitted.
	 */
	template<class Handler>
	void async_write_some(boost::asio::ip::tcp::socket& socket, const Handler& handler)
	{
		if (front_)
		{
			ssize_t rv = front_->write_some(socket);

			if (rv != -1)
			{
				size_ -= rv;

				if (!front_->size)
				{
					remove_front();
				}
			}
		}

		socket.async_write_some(boost::asio::null_buffers(), handler);
	}

	chunk *front_;
	chunk *back_;
	size_t size_;

public:
	/** initializes a composite buffer. */
	composite_buffer();

	/** initializes this composite buffer with chunks from  \p v by taking over ownership of \p v's chunks.
	 *
	 * \param v the other composite buffer to take over the chunks from.
	 *
	 * \note After this initialization, the other composite buffer \p v does not contain any chunks anymore.
	 */
	composite_buffer(const composite_buffer& v);

	~composite_buffer();

	/** assigns this composite buffer with chunks from  \p v by taking over ownership of \p v's chunks.
	 *
	 * \param v the other composite buffer to take over the chunks from.
	 *
	 * Current chunks within this composite buffer will be freed.
	 *
	 * \note After this initialization, the other composite buffer \p v does not contain any chunks anymore.
	 */
	composite_buffer& operator=(const composite_buffer& v);

	/** iterator, pointing to the front_ chunk. */
	iterator begin() const;

	/** iterator, representing the end of chunk list. */
	iterator end() const;

	/** removes first chunk in list if available. */
	void remove_front();

	/** retrieves first chunk in list. */
	chunk *front() const;

	/** retrieves last chunk in list. */
	chunk *back() const;

	/** retrieves the total number of bytes of all chunks. */
	size_t size() const;

	/** checks wether this composite buffer is empty or not. */
	bool empty() const;

	/** appends a character value. */
	void push_back(char value);

	/** appends a C-String value. */
	void push_back(const char *value);

	/** appends a string value. */
	void push_back(const std::string& value);

	/** appends a const buffer.
	 *
	 * \param buffer the const buffer
	 * \param size how many bytes of this const buffer to use.
	 *
	 * This buffer may <b>NOT</b> be destructed until used by this object, otherwise the sendto()
	 * may result into undefined behaviour.
	 */
	void push_back(const void *buffer, int size);

	/** appends a chunk that will read from given file descriptor on sendto().
	 *
	 * \param fd the file descriptor to read from on sendto()
	 * \param offset the offset inside the input file.
	 * \param size how many bytes to read from input and to pass to destination file.
	 * \param close indicates wether given file descriptor should be closed on chunk destruction.
	 *
	 * On sendto() this chunk may be optimized to directly pass the input buffers to the output buffers
	 * of their corresponding file descriptors by using sendfile() kernel system call.
	 */
	void push_back(int fd, off_t offset, size_t size, bool close);

	/** appends another composite buffer to this buffer.
	 *
	 * \param source the source buffer to append to this buffer.
	 *
	 * As performance matters, the source buffer will be directly integrated into the destination buffer,
	 * thus source.size() will be 0 after this operation.
	 */
	void push_back(composite_buffer& source);

	template<typename PodType, std::size_t N>
	void push_back(PodType (&data)[N]);

public:
	/** sends this buffer to given destnation.
	 * \param socket the socket to write to.
	 *
	 * Every chunk fully sent to the destination file is automatically removed from this buffer, 
	 * thus subsequent calls to sendto() will continue to sent the remaining chunks, if needed.
	 */
	ssize_t write(boost::asio::ip::tcp::socket& socket);

	/** sends this buffer <b>asynchronously</b> to given destnation.
	 * \param socket the socket to write to.
	 * \param handler the completion handler to call once all data has been sent (or an error occured).
	 *
	 * Every chunk fully sent to the destination file is automatically removed from this buffer, 
	 * thus subsequent calls to sendto() will continue to sent the remaining chunks, if needed.
	 */
	template<typename CompletionHandler>
	void async_write(boost::asio::ip::tcp::socket& socket, const CompletionHandler& handler);
};

// {{{ composite_buffer implementation
inline composite_buffer::composite_buffer() :
	front_(0), back_(0), size_(0)
{
}

inline composite_buffer::composite_buffer(const composite_buffer& v) :
	front_(v.front_), back_(v.back_), size_(v.size_)
{
	const_cast<composite_buffer *>(&v)->front_ = 0;
	const_cast<composite_buffer *>(&v)->back_ = 0;
	const_cast<composite_buffer *>(&v)->size_ = 0;
}

inline composite_buffer& composite_buffer::operator=(const composite_buffer& v)
{
	front_ = v.front_;
	back_ = v.back_;
	size_ = v.size_;

	const_cast<composite_buffer *>(&v)->front_ = 0;
	const_cast<composite_buffer *>(&v)->back_ = 0;
	const_cast<composite_buffer *>(&v)->size_ = 0;

	return *this;
}

inline composite_buffer::~composite_buffer()
{
	delete front_;
}

inline composite_buffer::iterator composite_buffer::begin() const
{
	return iterator(front_);
}

inline composite_buffer::iterator composite_buffer::end() const
{
	return iterator(0);
}

inline void composite_buffer::remove_front()
{
	if (front_)
	{
		chunk *new_front = front_->next;

		size_ -= front_->size;
		front_->next = 0;
		delete front_;

		front_ = new_front;
	}
}

inline composite_buffer::chunk *composite_buffer::front() const
{
	return front_;
}

inline composite_buffer::chunk *composite_buffer::back() const
{
	return back_;
}

inline size_t composite_buffer::size() const
{
	return size_;
}

inline bool composite_buffer::empty() const
{
	return !size_;
}

inline void composite_buffer::push_back(char value)
{
	push_back(&value, sizeof(value));
}

inline void composite_buffer::push_back(const char *value)
{
	push_back(value, std::strlen(value));
}

inline void composite_buffer::push_back(const std::string& value)
{
	if (back_)
	{
		if (back_->type == chunk::cstring)
		{
			static_cast<string_chunk *>(back_)->value += value;
		}
		else
		{
			back_->next = new string_chunk(value);
			back_ = back_->next;
			size_ += back_->size;
		}
	}
	else
	{
		front_ = back_ = new string_chunk(value);
		size_ = back_->size;
	}
}

inline void composite_buffer::push_back(const void *buffer, int size)
{
	if (!back_)
	{
		front_ = back_ = new iovec_chunk();
	}
	else if (back_->type != chunk::ciov)
	{
		back_->next = new iovec_chunk();
		back_ = back_->next;
	}

	static_cast<iovec_chunk *>(back_)->push_back(buffer, size);

	size_ += size;
}

template<typename PodType, std::size_t N>
inline void composite_buffer::push_back(PodType (&data)[N])
{
	push_back(static_cast<const void *>(data), N * sizeof(PodType));
}

inline void composite_buffer::push_back(int fd, off_t offset, size_t size, bool close)
{
	if (back_)
	{
		back_->next = new fd_chunk(fd, offset, size, close);
		back_ = back_->next;
	}
	else
	{
		front_ = back_ = new fd_chunk(fd, offset, size, close);
	}

	size_ += back_->size;
}

inline void composite_buffer::push_back(composite_buffer& buffer)
{
	if (back_)
	{
		back_->next = buffer.front_;
		back_ = buffer.back_;
		size_ += buffer.size_;
	}
	else
	{
		front_ = buffer.front_;
		back_ = buffer.back_;
		size_ = buffer.size_;
	}

	buffer.front_ = buffer.back_ = 0;
	buffer.size_ = 0;
}

inline ssize_t composite_buffer::write(boost::asio::ip::tcp::socket& socket)
{
	ssize_t rv, nwritten = 0;

	while (front_)
	{
		if ((rv = front_->write(socket)) != -1)
		{
			// XXX assume(rv == front_->size);
			nwritten += rv;

			chunk *next = front_->next;

			size_ -= front_->size;
			front_->next = 0;
			delete front_;

			front_ = next;
		}
		else
		{
			return -1;
		}
	}
	return nwritten;
}

template<typename CompletionHandler>
void composite_buffer::async_write(boost::asio::ip::tcp::socket& socket, const CompletionHandler& handler)
{
	write_handler<CompletionHandler> internalWriteHandler(*this, socket, handler);
	socket.async_write_some(boost::asio::null_buffers(), internalWriteHandler);
}
// }}}

} // namespace x0

#endif

















