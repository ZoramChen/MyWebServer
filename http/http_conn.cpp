#include "http_conn.h"
#include "../log/log.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker user_lock;
locker id_lock;
map<string, string> users;  //内存中的账户密码对
map<string, string> session_ids;  //内存中的sessionid用户名对

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;   // EPOLLIN：可读事件  EPOLLET：边缘触发  EPOLLRDHUP：对端关闭连接或关闭写端
    else
        event.events = EPOLLIN | EPOLLRDHUP;   // EPOLLIN：可读事件  EPOLLRDHUP：对端关闭连接或关闭写端

    if (one_shot)
        event.events |= EPOLLONESHOT;  // 该事件被触发一次后会被禁用，需用 epoll_ctl 重新启用
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        // 边缘触发：核心事件ev + 边缘触发 + 一次性触发 + 检测连接
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        // 水平触发：核心事件ev + 一次性触发 + 检测连接断开（无EPOLLET）
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        if(m_ssl_wrapper)
        {
            m_ssl_wrapper->shutdown();
            m_ssl_wrapper.reset();
        }
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}
//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     string user, string passwd, string sqlname, 
                     util_timer *time, sort_timer_lst *timer_lst,
                     int use_ssl, shared_ptr<SSLWrapper> ssl_wrapper)
{
    m_sockfd = sockfd;
    m_address = addr;

    m_TRIGMode = TRIGMode;
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    timer=time;
    timer_list=timer_lst;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    client_ip_ = inet_ntoa(addr.sin_addr);

    is_use_ssl = use_ssl;
    m_ssl_wrapper = ssl_wrapper;

    if (is_use_ssl)
        is_connect_success = true;
    else
        is_connect_success = false;


    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    // timer_flag = 0;

    m_session_id = "";
    m_is_logged_in = false;
    memset(m_session_id_buf, '\0', 65);
    m_has_session = false;
    m_need_set_cookie = false;

    m_file_size = 0;

    m_download_filename = NULL;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    // m_check_state != CHECK_STATE_CONTENT的作用是当read_once函数一次没有完整读完请求内容时，防止五度消息体内容以及修改
    for (; m_checked_idx < m_read_idx && m_check_state != CHECK_STATE_CONTENT; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {   
            //读到缓冲区末尾还没找到 \r\n，说明当前行不完整，返回 LINE_OPEN，等待后续数据。
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //出现非法的换行组合（比如单独的 \n 或 \r），返回 LINE_BAD，判定报文语法错误。
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
//将数据从内核态复制到用户态（m_read_buf）
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        if (is_use_ssl && is_connect_success)
        {
            try
            {
                bytes_read = m_ssl_wrapper->read(m_read_buf + m_read_idx,
                                                READ_BUFFER_SIZE - m_read_idx);
            }
            catch (const std::exception &e)
            {
                std::cerr << __FILE__ << " " << __LINE__ << " " << e.what() << '\n';
                LOG_ERROR("%s %d %s", __FILE__, __LINE__, e.what());
            }
        }
        else
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            if (is_use_ssl && is_connect_success)
            {
                try
                {
                    bytes_read = m_ssl_wrapper->read(m_read_buf + m_read_idx,
                                                    READ_BUFFER_SIZE - m_read_idx);
                }
                catch (const std::exception &e)
                {
                    std::cerr << __FILE__ << " " << __LINE__ << " " << e.what() << '\n';
                    LOG_ERROR("%s %d %s", __FILE__, __LINE__, e.what());
                }
            }
            else
                bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                //当前无数据可读
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            //对方关闭连接
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");  // 查找第一个空格或制表符
    if (!m_url)
    {
        return BAD_REQUEST;
    }  
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else if (strcasecmp(method, "PUT") == 0)
    {
        m_method = PUT;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");  //跳过 URL 前的空格
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{   
    //读到空行
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)  //strncasecmp 是一个 C 标准库函数，用于比较两个字符串的前 n 个字符，不区分大小写。
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "X-HTTP-Method-Override:", 22) == 0)
    {
        text += 22;
        // 跳过": ""
        text += 2;
        text += strspn(text, " \t");
        // 存储方法覆盖值
        m_method_override = text;
        LOG_INFO("Got method override: %s", text);
    }
    else if (strncasecmp(text, "X-Chunk-Number:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        chunk_header = atoi(text);
        LOG_INFO("Got chunk number: %d", chunk_header);
    }
    else if (strncasecmp(text, "X-Total-Chunks:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        total_header = atoi(text);
        LOG_INFO("Got total chunks: %d", total_header);
    }
    else if (strncasecmp(text, "X-File-Name:", 12) == 0)
    {
        text += 12;
        text += strspn(text, " \t");
        m_upload_filename = URLDecoder::decodeMixed(text);
        LOG_INFO("Got upload filename: %s", m_upload_filename.c_str());
    }
    else if (strncasecmp(text, "X-File-Size:", 12) == 0)
    {
        text += 12;
        text += strspn(text, " \t");
        m_file_size = atol(text);
        LOG_INFO("Got file size: %ld", m_file_size);
    } 
    else if (strncasecmp(text, "Cookie:", 7) == 0)
    {
        // text 形如 "Cookie: a=1; session_id=abcd...; b=2"
        const char *p = text + 7;
        p += strspn(p, " \t");
        const char *sid = strcasestr(p, "session_id=");
        if (sid)
        {
            sid += 11; // 跳过 "session_id="
            size_t n = 0;
            while (sid[n] && sid[n] != ';' && sid[n] != ' ' && n < 64)
                n++;
            // 只接受 32 位十六进制
            if (n == 32)
            {
                bool ok = true;
                // 确保session id是合法的
                for (size_t i = 0; i < 32; ++i)
                {
                    char c = sid[i];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                {
                    memcpy(m_session_id_buf, sid, 32);
                    m_session_id_buf[32] = '\0';
                    m_has_session = true;
                }
            }
        }
        if (strlen(m_session_id_buf) > 0)
        {
            m_has_session = true;
            m_session_id = std::string(m_session_id_buf);
        }
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{   
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);

        //主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                //解析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:
            {
                //解析请求头
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                else if (ret == GET_REQUEST) //获取到请求头全部信息
                {
                    return do_request();
                }
                break;
            }
            // 仅用于解析POST请求，调用parse_content函数解析消息体
            case CHECK_STATE_CONTENT:
            {   
                //解析消息体
                ret = parse_content(text);
                if (ret == GET_REQUEST)
                    return do_request();

                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');    // strrchr 是一个 C 标准库函数，用于在字符串中查找指定字符最后一次出现的位置。


    if (*(p + 1) != '0' && *(p + 1) != '1' && *(p + 1) != '2' && *(p + 1) != '3' && strcmp(p + 1, "judge.html") != 0)
    {
        if (m_has_session && validate_enhanced_session(m_session_id))
        {
            m_is_logged_in = true;
            LOG_INFO("Valid session attempt: %s", m_session_id.c_str());
        }
        else
        {
            // 1. 记录日志
            LOG_INFO("Invalid session attempt: %s", m_session_id.c_str());
            // 验证失败时的处理
            m_is_logged_in = false;
            // 2. 清除无效的session
            m_session_id.clear();
            // 3. 可以重定向到登录页面并显示提示信息
            strcpy(m_url, "/logTimeout.html");
        }
    }


    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUESn(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            
            user_lock.lock();
            if (users.find(name) == users.end())
            {
                
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                user_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else{
                user_lock.unlock();
                strcpy(m_url, "/registerError.html");
            }
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
            {
                string session_id;
                if (create_enhanced_session(name, session_id))
                {
                    LOG_INFO("User %s logged in successfully from %s",
                             name, client_ip_.c_str());
                    strcpy(m_url, "/welcome.html");
                    id_lock.lock();
                    session_ids[session_id]=name;
                    id_lock.unlock();

                }
                else
                {
                    strcpy(m_url, "/logError.html");
                }
            }
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5' && m_is_logged_in)
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6' && m_is_logged_in)
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7' && m_is_logged_in)
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '8' && m_is_logged_in)
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/upload.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '9' && m_is_logged_in)
    {
        string filename = URLDecoder::decodeMixed(m_url + 2);
        UploadFile up_file(this->doc_root, 1);
        int chunk_num = chunk_header, total_chunks = total_header;

        // 保存分块或完整文件
        bool save_result = false;
        if (total_chunks > 1)
        {
            // 分块上传文件
            save_result = up_file.save_uploaded_chunk(filename.c_str(), m_string, m_content_length,
                                                      chunk_num, total_chunks);

            // 如果是最后一个分块，合并文件
            if (save_result && chunk_num == total_chunks - 1)
            {
                if (session_ids.find(m_session_id) != session_ids.end()){
                    save_result = up_file.merge_uploaded_file(filename.c_str(), session_ids[m_session_id], total_chunks);
                }
                else
                {
                    up_file.cleanup_chunks(filename.c_str(), total_chunks);
                    LOG_WARN("do not find session id %s, delete %d %s file chunks\n", m_session_id.c_str(), total_chunks, filename);
                }
            }
        }
        else
        {
            // 单块直接保存
            if (session_ids.find(m_session_id) != session_ids.end()){
                save_result = up_file.save_uploaded_file(filename.c_str(), session_ids[m_session_id], m_string, m_content_length);
            }
            else
            {
                LOG_WARN("do not find session id %s, do not save file %s\n", m_session_id.c_str(), filename.c_str());
            }
        }

        return POST_REQUEST;

    }
    else if (*(p + 1) == 'a' && m_is_logged_in)
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/download.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == 'b' && m_is_logged_in)
    {
        if (session_ids.find(m_session_id) == session_ids.end())
        {
            LOG_WARN("do not find session id %s", m_session_id);
            return NO_RESOURCE;
        }
        char dir_path[FILENAME_LEN];
        string user_name = session_ids[m_session_id];
        snprintf(dir_path, sizeof(dir_path), "%s/uploads/%s", doc_root, user_name.c_str());
        DIR *dir = opendir(dir_path);
        if (!dir)
            return NO_RESOURCE;

        // 将列举出来的目录结果作为响应内容发送给浏览器（客户端）给显示出来
        struct dirent *entry;
        std::string json = "[";
        while ((entry = readdir(dir)) != NULL)
        {
            if (entry->d_type == DT_REG)
            {
                if (json.size() > 1)
                    json += ",";

                std::string fullpath = std::string(dir_path) + "/" + entry->d_name;
                struct stat fileStat;
                stat(fullpath.c_str(), &fileStat);

                char dateBuf[64];
                strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", localtime(&fileStat.st_mtime));

                json += "{";
                json += "\"name\":\"" + std::string(entry->d_name) + "\",";
                json += "\"size\":" + std::to_string(fileStat.st_size) + ",";
                json += "\"lastModified\":\"" + std::string(dateBuf) + "\"";
                json += "}";
            }
        }
        json += "]";
        closedir(dir);

        add_status_line(200, ok_200_title);
        add_headers(json.size());
        add_content(json.c_str());
        return NO_RESOURCE;
    }
    else if (*(p + 1) == 'c' && m_is_logged_in)
    {
        string filename = URLDecoder::decodeMixed(m_url + 2);

        if (session_ids.find(m_session_id) == session_ids.end())
        {
            LOG_WARN("do not find session id %s", m_session_id);
            return NO_RESOURCE;
        }

        char filepath[FILENAME_LEN];
        snprintf(filepath, sizeof(filepath), "%s/uploads/%s/%s", doc_root, session_ids[m_session_id].c_str(), filename.c_str());


        if (stat(filepath, &m_file_stat) < 0)
        {
            if (errno == ENOENT)
            {
                LOG_ERROR("File not found: %s", filepath);
            }
            else
            {
                LOG_ERROR("Cannot access file %s: %s", filepath, strerror(errno));
            }
            return NO_RESOURCE;
        }

        // 检查是否是目录
        if (S_ISDIR(m_file_stat.st_mode))
        {
            LOG_ERROR("Path is a directory: %s", filepath);
            return BAD_REQUEST;
        }

        strcpy(m_real_file, filepath);

        m_download_filename = m_url + 2;


    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 文件不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 没有读权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 路径1为目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    //通过「内存映射（mmap）」机制，将指定文件（m_real_file）的全部内容映射到进程的虚拟内存空间
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {

        if (is_use_ssl && is_connect_success)
        {
            try
            {
                temp = m_ssl_wrapper->write(m_iv, m_iv_count);
            }
            catch (const std::exception &e)
            {
                std::cerr << __FILE__ << " " << __LINE__ << " " << e.what() << '\n';
                LOG_ERROR("%s %d %s", __FILE__, __LINE__, e.what());
            }
            bytes_to_send -= temp;
        }
        else
        {
            temp = writev(m_sockfd, m_iv, m_iv_count);
        }


        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        //writev以顺序iov[0]，iov[1]
        //当写入的长度超过iov[0]，则修改iov[1]
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //当写入的长度没超过iov[0]，则修改iov[0]
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::add_content_disposition(const char *filename)
{
    return add_response("Content-Disposition: attachment; filename=\"%s\"\r\n", filename);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form))
                return false;
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        case POST_REQUEST:
        {
            add_status_line(200, ok_200_title);
            add_headers(0);
            if (!add_content(""))
                return false;
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line(200, ok_200_title);

            if (m_download_filename != NULL)
            {
                add_content_disposition(m_download_filename);
                m_download_filename = NULL;
            }


            if (m_file_stat.st_size != 0)
            {
                if (m_need_set_cookie)
                {
                    add_response("Set-Cookie: session_id=%s; Path=/; HttpOnly; SamaSite=Lax\r\n", m_session_id.c_str());
                    m_need_set_cookie = false;
                }
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        case NO_RESOURCE:
        {
            break;
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}



void http_conn::deal_timer()
{
    if(!timer)
    {
        LOG_INFO("close fd %d", m_sockfd);
        return;
    }
    int tmp_sockfd = timer->user_data->sockfd;
    if(timer->cb_func != NULL) {
        timer->cb_func(timer->user_data);
    }
    if (timer)
    {
        // printf("子线程删除 %p\n", (void*)timer);
        timer_list->del_timer(timer);
    }
    close(tmp_sockfd);

    LOG_INFO("close fd %d", m_sockfd);
}



bool http_conn::create_enhanced_session(const std::string &username, string &session_id)
{
    session_id = SessionManager::instance().create_session(
        username, client_ip_, ntohs(m_address.sin_port), user_agent_);

    if (!session_id.empty())
    {
        m_session_id = session_id;
        m_has_session = true;
        m_need_set_cookie = true;
        current_session_ = SessionManager::instance().get_session(session_id);
        return true;
    }
    return false;
}


bool http_conn::validate_enhanced_session(const std::string &session_id)
{
    if (SessionManager::instance().validate_session(session_id, client_ip_, user_agent_))
    {
        current_session_ = SessionManager::instance().get_session(session_id);
        m_is_logged_in = true;
        return true;
    }
    return false;
}
