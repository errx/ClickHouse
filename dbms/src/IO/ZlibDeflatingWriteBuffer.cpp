#include <IO/ZlibDeflatingWriteBuffer.h>
#include <Common/MemorySanitizer.h>


namespace DB
{

ZlibDeflatingWriteBuffer::ZlibDeflatingWriteBuffer(
        std::unique_ptr<WriteBuffer> out_,
        CompressionMethod compression_method,
        int compression_level,
        size_t buf_size,
        char * existing_memory,
        size_t alignment)
    : BufferWithOwnMemory<WriteBuffer>(buf_size, existing_memory, alignment)
    , out(std::move(out_))
{
    zstr.zalloc = nullptr;
    zstr.zfree = nullptr;
    zstr.opaque = nullptr;
    zstr.next_in = nullptr;
    zstr.avail_in = 0;
    zstr.next_out = nullptr;
    zstr.avail_out = 0;

    int window_bits = 15;
    if (compression_method == CompressionMethod::Gzip)
    {
        window_bits += 16;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    int rc = deflateInit2(&zstr, compression_level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
#pragma GCC diagnostic pop

    if (rc != Z_OK)
        throw Exception(std::string("deflateInit2 failed: ") + zError(rc) + "; zlib version: " + ZLIB_VERSION, ErrorCodes::ZLIB_DEFLATE_FAILED);
}

ZlibDeflatingWriteBuffer::~ZlibDeflatingWriteBuffer()
{
    try
    {
        finish();

        int rc = deflateEnd(&zstr);
        if (rc != Z_OK)
            throw Exception(std::string("deflateEnd failed: ") + zError(rc), ErrorCodes::ZLIB_DEFLATE_FAILED);
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

void ZlibDeflatingWriteBuffer::nextImpl()
{
    if (!offset())
        return;

    zstr.next_in = reinterpret_cast<unsigned char *>(working_buffer.begin());
    zstr.avail_in = offset();

    do
    {
        out->nextIfAtEnd();
        zstr.next_out = reinterpret_cast<unsigned char *>(out->position());
        zstr.avail_out = out->buffer().end() - out->position();

        int rc = deflate(&zstr, Z_NO_FLUSH);
        out->position() = out->buffer().end() - zstr.avail_out;

        // Unpoison the result of deflate explicitly. It uses some custom SSE algo
        // for computing CRC32, and it looks like msan is unable to comprehend
        // it fully, so it complains about the resulting value depending on the
        // uninitialized padding of the input buffer.
        __msan_unpoison(out->position(), zstr.avail_out);

        if (rc != Z_OK)
            throw Exception(std::string("deflate failed: ") + zError(rc), ErrorCodes::ZLIB_DEFLATE_FAILED);
    }
    while (zstr.avail_in > 0 || zstr.avail_out == 0);
}

void ZlibDeflatingWriteBuffer::finish()
{
    if (finished)
        return;

    next();

    while (true)
    {
        out->nextIfAtEnd();
        zstr.next_out = reinterpret_cast<unsigned char *>(out->position());
        zstr.avail_out = out->buffer().end() - out->position();

        int rc = deflate(&zstr, Z_FINISH);
        out->position() = out->buffer().end() - zstr.avail_out;

        // Unpoison the result of deflate explicitly. It uses some custom SSE algo
        // for computing CRC32, and it looks like msan is unable to comprehend
        // it fully, so it complains about the resulting value depending on the
        // uninitialized padding of the input buffer.
        __msan_unpoison(out->position(), zstr.avail_out);

        if (rc == Z_STREAM_END)
        {
            finished = true;
            return;
        }

        if (rc != Z_OK)
            throw Exception(std::string("deflate finish failed: ") + zError(rc), ErrorCodes::ZLIB_DEFLATE_FAILED);
    }
}

}
