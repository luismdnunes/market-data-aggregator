#include <random>
#include <stdlib.h>   // malloc

// NOTE: Add support to mmap
// #include <sys/mman.h> // mmap

/** INFO: This is the CPU architecture from the dev system
 *  NAME ONE-SIZE ALL-SIZE WAYS TYPE        LEVEL SETS PHY-LINE COHERENCY-SIZE
 *  L1d       32K     128K    8 Data            1   64        1             64
 *  L1i       32K     128K    8 Instruction     1   64        1             64
 *  L2       256K       1M    4 Unified         2 1024        1             64
 *  L3         6M       6M   12 Unified         3 8192        1             64
 *
 **/

/** INFO:
 * Use the feature flags to build different ways to test your hipotesys
 *
 */
#define FLAG_ORDERTICK_C  true

// size = 16
// align = 8
#if FLAG_ORDERTICK_C
struct OrderTick {
  uint64_t timestamp; // in nanoseconds
  uint32_t price;     // Fixed-point (with 2 decimal points)
  uint32_t volume;
 };
#else
struct OrderTick {
  uint64_t timestamp; // in nanoseconds
  uint32_t price;     // Fixed-point (with 2 decimal points)
  uint32_t volume;

  // NOTE: Disabled c/dtor and copy semantics
  //
  // Constructor
  OrderTick(/* args */) = delete;
  // Destructor
  ~OrderTick() = delete;
  // Copy constructor
  OrderTick(const OrderTick& other) = delete;
  // Copy assignment
  OrderTick& operator=(const OrderTick& other) = delete;
  // Move constructor
  OrderTick(OrderTick&& other) noexcept = default;
  // Move assignment
  OrderTick& operator=(OrderTick&& other) noexcept = default;
};
#endif

class OrderTickGenerator {
  private:
    OrderTick* tick;
    uint64_t current_time;
    uint32_t current_price;
    // Generators
    std::mt19937_64 generator;
    std::normal_distribution<double> price_dist;
    std::uniform_int_distribution<uint64_t> time_dist;
    std::uniform_int_distribution<uint32_t> volume_dist;

  public:
    explicit OrderTickGenerator(uint32_t seed, uint32_t start_price)
    : generator(seed),
      current_time(1719572400000000000ULL),
      current_price(start_price),
      time_dist(100, 50000),  // 100ns-50'000ns (50us)
      price_dist(0.0, 5.0),
      volume_dist(10, 1000)
    {
      // NOTE: Shared object pool / memory arena - store the lifetime of this object
      tick = static_cast<OrderTick*>(malloc(sizeof(OrderTick)));
    }

    ~OrderTickGenerator() { free(tick); }

    [[nodiscard]] OrderTick&& generate() noexcept {
      current_time += time_dist(generator);
      double price = price_dist(generator) * 100;
      uint32_t price_abs = static_cast<uint32_t>(price);

      // NOTE: No overflow or underflow checks
      if(price < 0) current_price -= price_abs;
      else current_price += price_abs;

      // Placeholder for the data that will be moved
      // WARN: Performance could be improved with emplace @ memory - one less move
      {
        tick->timestamp = current_time;
        tick->price = current_price;
        tick->volume = volume_dist(generator);
      };

      return std::move(*tick);
    }
};

// ---------------- OrderTickGenerator ------------------

// NOTE: Can add a template IF to add a destructor to free the memory when member is deleted
template<typename T, std::size_t N>
struct RingBuffer {
  T* buffer;
  std::size_t head;
  std::size_t tail;
 
  RingBuffer() {
    buffer = static_cast<T*>(malloc(sizeof(T) * N));
  }

  // NOTE: Destructor disabled as this memory will be freed when process will be deleted
  // ~RingBuffer() {}

  // NOTE: Improve copy to move or build
  T* push(T&& element){
    if(is_full()) {
      return nullptr;
    }

    buffer[tail%N] = std::move(element);
    ++tail;

    return &buffer[(tail-1)%N];
  }

  void pop(){
    if(is_empty()) {
      return;
    }

    // If we don't need to return the element, we can just skip it
    // T&& element = std::move(buffer[head%N]);
    ++head;

    // Add check bounds and trim size counters - Avoid overflow
    const std::size_t head_chunk = head/N;
    const std::size_t tail_chunk = tail/N;
    if(head_chunk && head_chunk==tail_chunk) {
       head-=N;
       tail-=N;
    }
  }

  T* peek() {
    if(is_empty()) {
      return nullptr;
    }

    return &buffer[head%N];
  }

  void reset() { head = tail; }
  constexpr std::size_t capacity() { return N; }
  uint32_t size() { return tail-head; }
  bool is_full() const { return tail-head == N; }
  bool is_empty() const { return head == tail; }
};

template<typename T, uint64_t BUFFER_SIZE, uint64_t TIME_WINDOW>
struct TimeWindow {
  RingBuffer<T, BUFFER_SIZE> buffer;
  uint64_t timestamp; // in nanoseconds
 
  struct {
    uint32_t dropped;               // Dropped from the window
    uint32_t max_traded_volume;     // max of all sum(volume)
    uint32_t window_price_volume;   // current sum(price x volume)
    uint32_t traded_volume;         // current sum(volume)
    uint32_t max_window_size;       // max window size achieved during run
  } counters;

  bool process_tick(OrderTick&& tick) noexcept {
    // NOTE: This assumes the tick has the latest timestamp
    timestamp = tick.timestamp;

    // Cleaning up the timewindow
    uint64_t drop_timestamp = timestamp - TIME_WINDOW;

    while(!buffer.is_empty() ){
      auto _p = buffer.peek();
      // Clipped
      if(_p && drop_timestamp >= _p->timestamp) {
        counters.traded_volume -= _p->volume;
        counters.window_price_volume -= _p->price * _p->volume;
        buffer.pop();
        continue;
      }
      break;
    }

    OrderTick const* const _tick = buffer.push(std::move(tick));
    if(_tick == nullptr) {
     ++counters.dropped;
     return false;
    }

    // Update counters
    counters.traded_volume += _tick->volume;
    counters.window_price_volume += _tick->price * _tick->volume;
    counters.max_traded_volume = std::max(counters.max_traded_volume, counters.traded_volume);
    counters.max_window_size = std::max(counters.max_window_size, buffer.size());

    return true;
  }

  // API:
  uint32_t get_max_traded_volume() { return counters.max_traded_volume; }
  uint32_t get_max_window_size() { return counters.max_window_size; }
  double get_volume_weighted_average() { return (double)counters.window_price_volume/(double)counters.traded_volume; }
};


// -------------------------


// This is the example underneath
#include <cstdio>
#include <print>
#include <vector>
// Definitions
#define N_TICKS 10'000'000

template<typename T>
void print_data_info() {
  std::println("sizeof({})  = {}", typeid(T).name(), sizeof(T));
  std::println("alignof({}) = {}", typeid(T).name(), alignof(T));
}

int main (int argc, char *argv[]) {
  // Initial settings
  // const uint64_t init_ts{ 1719572400000000000ULL };
  const uint32_t seed{ 69 };
  const uint32_t start_price{ 10000 };
  //
  auto generator = OrderTickGenerator(seed, start_price);
 

  // TimeWindow
  const std::size_t BUFFER_SIZE{ 500'000 };         // [in # ticks] - 500'000 Order Ticks
  const std::size_t TIME_WINDOW{ 10'000'000'000 };  // [in ns]      - 10 s = 10'000 ms = 10'000'000 us = 10'000'000'000 ns
                                                    //
  TimeWindow<OrderTick, BUFFER_SIZE, TIME_WINDOW> window;

  // NOTE: Main loop - data generation
  for(int i=0; i<N_TICKS; ++i) {
    window.process_tick(generator.generate());
  }

  std::println("------------------------");

  // TODO: Print window statistics
  std::println("Window size: {}", window.buffer.tail-window.buffer.head);
  std::println("Window size: {}", TIME_WINDOW);
  std::println("Buffer size: {}", BUFFER_SIZE);
  std::println("Statistics:");
  std::println("+ Max Window Size         : {}", window.get_max_window_size());
  std::println("-- ");
  std::println("+ Dropped ticks           : {}", window.counters.dropped);
  std::println("+ Traded Volume           : {}", window.counters.traded_volume);
  std::println("+ Max Traded Volume       : {}", window.get_max_traded_volume());
  std::println("+ Volume-Weighted Average : {}", window.get_volume_weighted_average());
  std::println("-- ");

  // NOTE: Extra stats
  uint32_t _traded_volume{ 0 };
  uint32_t _max_traded_volume{ 0 };
  uint32_t _window_price_volume{ 0 };
  for(int i=0; !window.buffer.is_empty(); ++i) {
    auto const pTick = window.buffer.peek();
    // std::println("|{}: [{}] - {} @ {}", i, tick.timestamp, tick.volume, tick.price);

    // Extra stats
    _traded_volume+=pTick->volume;
    _max_traded_volume=std::max(_max_traded_volume, _traded_volume);
    _window_price_volume+=pTick->volume * pTick->price;
    window.buffer.pop();
  }
  std::println("-- ");
  std::println("* Traded Volume           : {}", _traded_volume);
  std::println("* Max Traded Volume       : {}", _max_traded_volume);
  std::println("* Volume-Weighted Average : {}", (double) _window_price_volume / _traded_volume);

  return 0;
}
