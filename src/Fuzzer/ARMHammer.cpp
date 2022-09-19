// replaces CodeJitter with our normal hammering code

#define CUTOFF_TIME_NS = 300

// rewrite of CodeJitter::jit_strict for ARM with DMA
void hammer(int num_acts_per_trefi,
            const std::vector<volatile char *> & aggressor_pairs,
            bool sync_each_ref,
            int num_aggressors_for_sync,
            int total_num_activations) {

  // make sure we have enough aggressors to use some to sync
  if (static_cast<size_t>(num_aggressors_for_sync) > aggressor_pairs.size()) {
    Logger::log_error(format_string("NUM_TIMED_ACCESSES (%d) is larger than #aggressor_pairs (%zu).",
        num_aggressors_for_sync,
        aggressor_pairs.size()));
    return;
  }
  
  // 1: synchronize with the beginning of an interval
	uint64_t td = 0;
	while (td < CUTOFF_TIME_NS)
	{
		uint64_t rt0 = realtime_now();

    // use the first num_aggressors_for_sync addresses to sync with refresh interval
    for (int i = 0; i < num_aggressors_for_sync; i++) {
      *(volatile char *)aggressor_pairs[i];
    }
		asm volatile("isb");

		uint64_t rt1 = realtime_now();
		td = rt1 - rt0;
		Logger::log_debug("Waiting for refresh; access time = %llu", td);
	}

  // 2: perform hammering
  for (int act = 0; act < total_num_activations; act++) {
    // hammer each aggressor once
    size_t cnt_total_activations = 0;

    for (int i = num_aggressors_for_sync; i < static_cast<int>(aggressor_pairs.size()) - num_aggressors_for_sync; i++) {
      *(volatile char *)aggressor_pairs[i];
      cnt_total_activations++;

      if (sync_each_ref
          && ((cnt_total_activations%num_acts_per_trefi)==0)) {
        std::vector<volatile char *> aggs(aggressor_pairs.begin() + i,
            std::min(aggressor_pairs.begin() + i + num_aggressors_for_sync, aggressor_pairs.end()));
        sync_ref(aggs, a);
      }
    }

    asm volatile("isb"); // fencing

    // 3: synchronize with the end
    // use the last num_aggressors_for_sync addresses to sync at the end of the pattern
    std::vector<volatile char *> last_aggs(aggressor_pairs.end() - NUM_TIMED_ACCESSES, aggressor_pairs.end());
    sync_ref(last_aggs, a);
  }

}
void sync_ref(const std::vector<volatile char *> &aggressor_pairs) {
	uint64_t td = 0;
	while (td < CUTOFF_TIME_NS)
	{
    asm volatile("isb");
		uint64_t rt0 = realtime_now();

    // use the first num_aggressors_for_sync addresses to sync with refresh interval
    for (auto agg : aggressor_pairs) {
      *(volatile char *)aggressor_pairs[i];
    }
		asm volatile("isb");

		uint64_t rt1 = realtime_now();
		td = rt1 - rt0;
	}
}