#include "ssl_wrapper.h"
#include <stdexcept>

SSLWrapper::SSLWrapper(int sockfd, SSL_CTX *ctx)
    : ctx_(ctx), ssl_(nullptr), sockfd_(sockfd), writeBio_(nullptr)
{
    if (!ctx_)
        throw std::runtime_error("SSL_CTX is null");

    ssl_ = SSL_new(ctx_);
    if (!ssl_)
        throw_ssl_error("SSL_new failed");

    // ===== 使用 socket BIO，由 OpenSSL 自己 recv =====
    BIO *bio = BIO_new_socket(sockfd_, BIO_NOCLOSE);
    if (!bio)
        throw std::runtime_error("BIO_new_socket failed");

    SSL_set_bio(ssl_, bio, bio);
    SSL_set_accept_state(ssl_);
}

SSLWrapper::~SSLWrapper()
{
    if (ssl_)
    {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
}

bool SSLWrapper::accept()
{
    int ret = SSL_accept(ssl_);
    if (ret == 1)
        return true;

    int err = SSL_get_error(ssl_, ret);

    switch (err)
    {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
        // 非阻塞 socket：等待下次事件
        return false;

    case SSL_ERROR_ZERO_RETURN:
        // 对端正常关闭
        return false;

    case SSL_ERROR_SSL:
    {
        unsigned long e = ERR_peek_error();
        if (ERR_GET_REASON(e) == SSL_R_HTTP_REQUEST)
        {
            // 明文 HTTP 请求打到 HTTPS 端口
            return false;
        }
        print_detailed_ssl_errors(ssl_, ret);
        return false;
    }

    default:
        print_detailed_ssl_errors(ssl_, ret);
        return false;
    }
}

int SSLWrapper::read(void *buf, size_t len)
{
    int n = SSL_read(ssl_, buf, len);
    if (n <= 0)
    {
        int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_ZERO_RETURN)
            return 0;
        return -1;
    }
    return n;
}

int SSLWrapper::write(const struct iovec *iov, int iovcnt)
{
    int total = 0;
    for (int i = 0; i < iovcnt; ++i)
    {
        const char *p = static_cast<const char *>(iov[i].iov_base);
        size_t left = iov[i].iov_len;

        while (left > 0)
        {
            int n = SSL_write(ssl_, p, left);
            if (n <= 0)
            {
                int err = SSL_get_error(ssl_, n);
                if (err == SSL_ERROR_WANT_WRITE)
                    continue;
                return -1;
            }
            p += n;
            left -= n;
            total += n;
        }
    }
    return total;
}

void SSLWrapper::shutdown()
{
    if (ssl_)
        SSL_shutdown(ssl_);
}

void SSLWrapper::print_detailed_ssl_errors(SSL *ssl, int ret)
{
    unsigned long e;
    char buf[256];

    while ((e = ERR_get_error()) != 0)
    {
        ERR_error_string_n(e, buf, sizeof(buf));

        // ⭐ OpenSSL 3.x：远端证书不信任 alert（忽略）
        if (strstr(buf, "certificate unknown") != nullptr)
            continue;

        std::cerr << "OpenSSL: " << buf << "\n";
    }
}

std::string SSLWrapper::get_last_error() const
{
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

void SSLWrapper::throw_ssl_error(const std::string &msg) const
{
    throw std::runtime_error(msg + ": " + get_last_error());
}
