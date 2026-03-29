#include <benchmark/benchmark.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

// Expose internal functions and globals from server.c
extern "C" {
    #include "logger.h"
    
    // Declare the cache struct structure to avoid incomplete type errors
    typedef struct cache cache;
    struct cache {
        char *data;
        size_t len;
        char* url;
        time_t time;
        cache* next;
    };

    extern pthread_mutex_t lock;
    cache* find(char *url);
    int addCache(char *data, size_t size, char *url);
    void removeCache();
}

// Define a Fixture to initialize mutexes before benchmarks run
class CacheFixture : public benchmark::Fixture {
public:
    void SetUp(const ::benchmark::State& state) {
        pthread_mutex_init(&lock, NULL);
        log_init();
    }
    void TearDown(const ::benchmark::State& state) {
        log_destroy();
        pthread_mutex_destroy(&lock);
    }
};

// Benchmark the addCache function
BENCHMARK_F(CacheFixture, BM_AddCache)(benchmark::State& state) {
    char url[] = "http://example.com";
    char data[] = "<html><body>Test</body></html>";
    
    for (auto _ : state) {
        addCache(data, strlen(data), url);
    }
}

// Benchmark the find function
BENCHMARK_F(CacheFixture, BM_FindCache)(benchmark::State& state) {
    char url[] = "http://example.com";
    
    for (auto _ : state) {
        benchmark::DoNotOptimize(find(url));
    }
}

BENCHMARK_MAIN();