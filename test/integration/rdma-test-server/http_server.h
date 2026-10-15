/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>

#include "unique-fd.h"

namespace hipobj::test {

struct HttpRequest {
    std::string                        method{};
    std::string                        path{};
    std::map<std::string, std::string> headers{}; /* lower-case names */
    std::string                        body{};
    size_t contentLength = 0; /* Content-Length, which HttpServer validates; 0 if absent */
};

struct HttpResponse {
    int                                status = 500;
    std::map<std::string, std::string> headers{};
    std::string                        body{};
};

using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

class HttpServer {
public:
    explicit HttpServer(int port);
    ~HttpServer();

    /* The accept loop and its workers hold this */
    HttpServer(const HttpServer &)            = delete;
    HttpServer &operator=(const HttpServer &) = delete;
    HttpServer(HttpServer &&)                 = delete;
    HttpServer &operator=(HttpServer &&)      = delete;

    void setHandler(HttpHandler handler);

    /* Accepts connections until stop(), handling each on its own
     * thread. The loop always runs on the tracked thread created
     * here, so stop() has a join barrier before waiting on the
     * worker count.
     *
     * Returns false when the constructor never got a listening socket.
     * Callers must not announce a port they are not listening on. */
    bool startThreaded();
    void stop();

    [[nodiscard]] int fd() const
    {
        return listen_fd_.get();
    }

private:
    /* A client connection handed to a worker thread. While it exists,
     * its fd is in activeFds_ and counted in liveWorkers_, so stop() can
     * shut the connection down and wait for the worker. The destructor
     * unregisters the fd before closing it, so accept() can't hand out
     * the same number while it's still registered. */
    class Worker {
    public:
        /* The caller must already have registered client */
        Worker(HttpServer &server, hipObj::UniqueFd client) noexcept;
        ~Worker();

        Worker(const Worker &)            = delete;
        Worker &operator=(const Worker &) = delete;
        Worker(Worker &&other) noexcept;
        Worker &operator=(Worker &&) = delete;

        [[nodiscard]] int fd() const
        {
            return client_.get();
        }

    private:
        HttpServer      *server_; /* nullptr once moved from */
        hipObj::UniqueFd client_;
    };

    /* The accept loop, which runs until stop is requested */
    void runThreaded(const std::stop_token &stop);
    /* Registers client for a worker, or returns nothing (closing client)
     * once stop is requested */
    std::optional<Worker> registerWorker(hipObj::UniqueFd client, const std::stop_token &stop);
    /* Serves one request on client, which the caller owns and closes */
    void handleConnection(int client);

    hipObj::UniqueFd listen_fd_;
    HttpHandler      handler_{};
    /* The accept loop runs on its own thread when started via
     * startThreaded(); stop() requests that it stop, and joins it as the
     * production barrier. Until then, it has no stop state to request. */
    std::jthread acceptLoop_{};
    std::mutex   workersMtx_{};
    /* Detached workers: stop() waits for this count to reach zero
     * after shutting down the registered fds. */
    int                     liveWorkers_ = 0;
    std::condition_variable workerDoneCv_{};
    /* Client fds with a live worker; guarded by workersMtx_. stop()
     * shutdowns these (via dup'd handles) to unblock workers. */
    std::set<int> activeFds_{};
};

HttpRequest parseRequest(const std::string &raw);
std::string serializeResponse(const HttpResponse &resp);

} // namespace hipobj::test
