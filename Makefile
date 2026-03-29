CC = gcc
CXX = g++

CFLAGS = -g -Wall -I./include
CXXFLAGS = -g -Wall -O3 -I./include
LDFLAGS = -lpthread

SERVER_TEST_FLAGS = -g -Wall -I./include -DBENCHMARKING

all: proxy

proxy: src/server.o src/proxy_parse.o src/logger.o
	$(CC) $(CFLAGS) -o proxy src/server.o src/proxy_parse.o src/logger.o $(LDFLAGS)

src/server.o: src/server.c include/proxy_parse.h include/logger.h
	$(CC) $(CFLAGS) -c src/server.c -o src/server.o

src/proxy_parse.o: src/proxy_parse.c include/proxy_parse.h
	$(CC) $(CFLAGS) -c src/proxy_parse.c -o src/proxy_parse.o

src/logger.o: src/logger.c include/logger.h
	$(CC) $(CFLAGS) -c src/logger.c -o src/logger.o


src/server_test.o: src/server.c include/proxy_parse.h include/logger.h
	$(CC) $(SERVER_TEST_FLAGS) -c src/server.c -o src/server_test.o

benchmark: benchmarks/benchmark_cache.cpp src/server_test.o src/proxy_parse.o src/logger.o
	$(CXX) $(CXXFLAGS) benchmarks/benchmark_cache.cpp src/server_test.o src/proxy_parse.o src/logger.o -o my_benchmark -lbenchmark $(LDFLAGS)

report: benchmark
	./my_benchmark --benchmark_out=benchmark_results.txt --benchmark_out_format=console

clean:
	rm -f proxy my_benchmark src/*.o benchmark_results.*