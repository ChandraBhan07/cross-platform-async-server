# Cross-Platform Async Server

A cross-platform asynchronous TCP server written in C++17, using **IOCP on Windows** and **epoll on Linux**.

The project explores event-driven networking, asynchronous I/O, message-driven architecture, concurrency and the separation of platform-specific transport from application logic.

## Motivation

My interest in this project started when I was working with Node.js. I was fascinated by its event loop and how a single-threaded execution model could handle a large number of concurrent connections without blocking on I/O.

I wanted to understand what actually happens beneath that abstraction. What makes an event loop work? How does the operating system notify an application about completed I/O? How are connections, buffers and concurrent operations managed?

That curiosity eventually led me to C++ and operating-system networking APIs.

I initially experimented with polling-based approaches, worker threads and session management. Building a cross-platform implementation turned out to be a much more interesting challenge than I expected, particularly because Windows and Linux expose fundamentally different I/O models.

This project is my attempt to understand and implement those mechanisms rather than simply use an existing networking abstraction.

## Architecture

The project is organized into three components:

- **Transport:** Platform-specific asynchronous TCP networking using Windows IOCP and Linux epoll.
- **Runtime:** Thread-safe queues and message-passing infrastructure connecting independent components.
- **Application:** Message dispatching and independent workers that process incoming frames.

Application processing remains independent of the underlying transport implementation.

## Current Features

- Cross-platform TCP networking (Windows and Linux).
- IOCP-based asynchronous I/O on Windows.
- Non-blocking sockets and epoll on Linux.
- Custom header-and-payload message framing.
- Session-aware message routing.
- Queue-based communication between transport and application components.
- Independent application worker threads.
- Partial transmission tracking.

## Status

**Work in progress.**

The current implementation includes a basic chat message worker and has been tested with up to 10 connected clients.

Future plans include additional application workers, video-related message processing, improved lifecycle management and further reliability testing.

This project is an ongoing exploration of networking, concurrency and systems programming in C++.
