# Yam::HTTP Library

***A simple to use, yet efficient, modern C++ HTTP server library***

This is a library for developing HTTP servers with custom request processing. \
It uses asynchronous I/O operations, relying on *epoll* (so it is Linux-only), and reaches some very good numbers in terms of performance. At the same time, it's very easy to set up, as shown below.

It has a light footprint, since it only uses a small external library, *fmtlib*, and the standard C++ library, not even relying on *Boost*. In addition, it employs modern C++ constructs, and compiles well under G++ 4.9.2 with `-std=c++14`.

### Feature Summary

- Asynchronous I/O
- Independent Read/Write throttling, both for the server as a whole and for individual connections
- Supports conditional message body fetching & rejection
- Supports cached responses for added efficiency
- Efficient, does not overload the CPU or memory when not required
- Lightweight, only depends on the C++ Standard Library and a small external static library for fast string formatting
- Uses modern C++ and is easy to use, and even to customize the code
- Public API documented with Doxygen
- Thoroughly tested with Google Test unit & integration tests, and also with Valgrind Memcheck

## Example Code (Hello World)

```c++
class HelloWorldChannel : public Channel {
    // Use constructors from Channel
    using Channel::Channel;

    // Process incoming requests
    Control Process() override {
        GetResponder().SetContent(CreateHtml());
        GetResponder().SetField("Content-Type", "text/html");
        return SendResponse(Status::Ok);
    }

    std::shared_ptr<std::vector<char>> CreateHtml() const {
        auto text = std::string("<u><b>Hello world!</b></u>\n");
        return std::make_shared<std::vector<char>>(begin(text), end(text));
    }
};

class HelloWorldChannelFactory : public ChannelFactory {
    std::unique_ptr<Channel> CreateChannel(std::shared_ptr<FileStream> fs) override {
        return std::make_unique<HelloWorldChannel>(std::move(fs));
    }
};

int main() {
    auto endpoint = IPEndpoint({127, 0, 0, 1}, 3000);
    auto factory = std::make_unique<HelloWorldChannelFactory>();
    auto processingThreads = 1;

    HttpServer server(endpoint, std::move(factory), processingThreads);
    Log::Default()->SetLevel(Log::Level::Info);
    server.Start().wait();
}
```

## Getting Started
### Install Build Prerequisites
Assuming you're on Ubuntu/Debian,

```bash
$ sudo apt-get install cmake libgtest-dev google-mock libunwind-dev
```
### Compile & Run

```bash
$ git clone https://github.com/ymarcov/http
$ cd http
$ git submodule update --init
$ cmake .
$ make hello_world
$ bin/hello_world
```

Then connect to `http://localhost:3000` from your browser.

## Documentation (Doxygen)
Documentation may be generated by running ```doxygen Doxyfile```. \
After that, it'll be available under `doc/html/index.html`.

## Performance
Running on Intel i7-4790 @ 3.6GHz with 8GB of memory, here are ApacheBench results, showing that this server can handle quite a few requests at a time, and is definitely performant enough for most use cases.

```bash
$ bin/sandbox_echo 3000 4 0 & # Run sandbox server with 4 threads
$ ab -c4 -n100000 http://127.0.0.1:3000/
```

```
This is ApacheBench, Version 2.3 <$Revision: 1604373 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient)
Finished 100000 requests

Server Hostname:        127.0.0.1
Server Port:            3000

Document Path:          /
Document Length:        26 bytes

Concurrency Level:      4
Time taken for tests:   3.228 seconds
Complete requests:      100000
Failed requests:        0
Total transferred:      8400000 bytes
HTML transferred:       2600000 bytes
Requests per second:    30974.65 [#/sec] (mean)
Time per request:       0.129 [ms] (mean)
Time per request:       0.032 [ms] (mean, across all concurrent requests)
Transfer rate:          2540.89 [Kbytes/sec] received
```

