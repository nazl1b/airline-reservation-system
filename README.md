# ✈️ Airline Reservation System

A multi-threaded TCP client/server system in C for searching and booking flights.
Team project for the Operating Systems course at the University of Thessaly.

## Features
- Search flights between two cities within a date range (direct and with one connection)
- Book a seat on a direct flight
- Book connecting flights: both seats are booked, or none (all-or-nothing)
- Many clients at the same time: each client is served by its own thread

## How it works
- **Sockets:** TCP over IPv6
- **Threads:** one thread per client (`pthread`)
- **Synchronization:** one mutex per flight (record-level locking), so different flights can be booked in parallel
- **Deadlock avoidance:** when two flights are locked, the smaller flight id is always locked first
- **Storage:** flights and reservations are saved in CSV files

## Tech
C · POSIX threads · TCP sockets · Makefile · Linux/macOS

## How to run
```bash
make
./server          # starts on port 5555
./client ::1 5555 # in another terminal
```
