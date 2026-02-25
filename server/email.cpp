// email.cpp
#include "email.h"
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstring>
#include <curl/curl.h>

// ==========================================
// [설정] 구글 앱 비밀번호 입력 필수
// ==========================================
static const std::string SMTP_URL = "smtps://smtp.gmail.com:465";
static const std::string SMTP_USER = "sleimneer@gmail.com";
static const std::string SMTP_PASS = "lqcq tdyh dsug ahfo";

// ==========================================
// [내부 전용] 구조체 및 변수 (외부 노출 X)
// ==========================================
struct EmailTask
{
    std::string to;
    std::string subject;
    std::string body;
};

static std::queue<EmailTask> g_queue;
static std::mutex g_mutex;
static std::condition_variable g_cv;
static std::atomic<bool> g_running(false);
static std::thread g_worker;

// ==========================================
// [내부 전용] libcurl 헬퍼 함수들
// ==========================================
struct UploadStatus
{
    const char *data;
    size_t size;
    size_t bytes_read;
};

static size_t payload_source(void *ptr, size_t size, size_t nmemb, void *userp)
{
    UploadStatus *upload_ctx = (UploadStatus *)userp;
    size_t room = size * nmemb;
    if ((size == 0) || (nmemb == 0) || ((size * nmemb) < 1))
        return 0;
    if (upload_ctx->size > 0)
    {
        size_t len = upload_ctx->size > room ? room : upload_ctx->size;
        memcpy(ptr, upload_ctx->data + upload_ctx->bytes_read, len);
        upload_ctx->bytes_read += len;
        upload_ctx->size -= len;
        return len;
    }
    return 0;
}

static void send_email_real(const std::string &to, const std::string &subject, const std::string &body)
{
    CURL *curl;
    CURLcode res = CURLE_OK;
    struct curl_slist *recipients = NULL;

    std::string payload_text =
        "To: " + to + "\r\n"
                      "From: " +
        SMTP_USER + "\r\n"
                    "Subject: " +
        subject + "\r\n"
                  "\r\n" +
        body + "\r\n";

    UploadStatus upload_ctx = {payload_text.c_str(), payload_text.size(), 0};

    curl = curl_easy_init();
    if (curl)
    {
        curl_easy_setopt(curl, CURLOPT_USERNAME, SMTP_USER.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, SMTP_PASS.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, SMTP_URL.c_str());
        curl_easy_setopt(curl, CURLOPT_USE_SSL, CURLUSESSL_ALL);
        curl_easy_setopt(curl, CURLOPT_MAIL_FROM, SMTP_USER.c_str());
        recipients = curl_slist_append(recipients, to.c_str());
        curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);

        res = curl_easy_perform(curl);

        if (res != CURLE_OK)
            std::cerr << "[Email Fail] " << curl_easy_strerror(res) << std::endl;
        else
            std::cout << "[Email Success] Sent to " << to << std::endl;

        curl_slist_free_all(recipients);
        curl_easy_cleanup(curl);
    }
}

static void worker_loop()
{
    while (g_running)
    {
        EmailTask task;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, []
                      { return !g_queue.empty() || !g_running; });
            if (!g_running && g_queue.empty())
                break;
            task = g_queue.front();
            g_queue.pop();
        }
        send_email_real(task.to, task.subject, task.body);
    }
}

// ==========================================
// [외부 공개] email.h 구현
// ==========================================
void email_init()
{
    if (g_running)
        return;
    g_running = true;
    g_worker = std::thread(worker_loop);
    g_worker.detach(); // 백그라운드에서 실행
}

void email_send(const std::string &to, const std::string &subject, const std::string &body)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push({to, subject, body});
    }
    g_cv.notify_one();
}

void email_shutdown()
{
    g_running = false;
    g_cv.notify_all();
    // detach 했으므로 join 불필요, 혹은 joinable 확인 후 join
}