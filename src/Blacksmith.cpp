#include "Blacksmith.hpp"

#include <sys/resource.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>

#include "Forges/TraditionalHammerer.hpp"
#include "Forges/FuzzyHammerer.hpp"

#include <argagg/argagg.hpp>
#include <argagg/convert/csv.hpp>

ProgramArguments program_args;

int check_cpu() {
  std::array<char, 128> buffer{};
  std::string cpu_model;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("cat /proc/cpuinfo | grep \"model name\" | cut -d':' -f2 | awk '{$1=$1;print}' | head -1", "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    cpu_model += buffer.data();
  }

  Logger::log_info("Detecting CPU model:");
  Logger::log_data(format_string("%s", cpu_model.c_str()));

  std::vector<std::string> supported_cpus = {
      // Coffee Lake
      "i5-8400",
      "i5-8500",
      "i5-8600",
      "i5-9400",
      "i5-9500",
      "i5-9600",
      "i7-8086",
      "i7-8700",
      "i7-9700",
      "i7-9900",
      "" // let it run on the raspberry pi
  };

  bool cpu_supported = false;
  for (const auto &model : supported_cpus) {
    cpu_supported |= (cpu_model.find(model) != std::string::npos);
  }

  if (!cpu_supported) {
    Logger::log_error("CPU model is not supported. You need to run DRAMA to update the DRAM address matrices. See the README.md for details.");
    exit(EXIT_FAILURE);
  }

  return cpu_supported;
}

int main(int argc, char **argv) {
  Logger::initialize();

  // check if the system's CPU is supported by our hard-coded DRAM address matrices
  check_cpu();

  handle_args(argc, argv);

  // prints the current git commit and some program metadata
  Logger::log_metadata(GIT_COMMIT_HASH, program_args.runtime_limit);

  // give this process the highest CPU priority so it can hammer with less interruptions
  int ret = setpriority(PRIO_PROCESS, 0, -20);
  if (ret!=0) Logger::log_error("Instruction setpriority failed.");

  // allocate a large bulk of contiguous memory
  Memory memory(true);
  memory.allocate_memory(MEM_SIZE);

  // find address sets that create bank conflicts
  DramAnalyzer dram_analyzer(memory.get_starting_address());
  dram_analyzer.find_bank_conflicts();
  if (program_args.num_ranks != 0) {
    dram_analyzer.load_known_functions(program_args.num_ranks);
  } else {
    Logger::log_error("Program argument '--ranks <integer>' was probably not passed. Cannot continue.");
    exit(EXIT_FAILURE);
  }
  // initialize the DRAMAddr class to load the proper memory configuration
  DRAMAddr::initialize(dram_analyzer.get_bank_rank_functions().size(), memory.get_starting_address());

  // count the number of possible activations per refresh interval, if not given as program argument
  if (program_args.acts_per_trefi==0)
    program_args.acts_per_trefi = dram_analyzer.count_acts_per_trefi();

  if (!program_args.load_json_filename.empty()) {
    ReplayingHammerer replayer(memory);
    if (program_args.sweeping) {
      replayer.replay_patterns_brief(program_args.load_json_filename, program_args.pattern_ids,
          MB(256), false);
    } else {
      replayer.replay_patterns(program_args.load_json_filename, program_args.pattern_ids);
    }
  } else if (program_args.do_fuzzing && program_args.use_synchronization) {
    FuzzyHammerer::n_sided_frequency_based_hammering(dram_analyzer, memory, static_cast<int>(program_args.acts_per_trefi), program_args.runtime_limit,
        program_args.num_address_mappings_per_pattern, program_args.sweeping);
  } else if (!program_args.do_fuzzing) {
//    TraditionalHammerer::n_sided_hammer(memory, program_args.acts_per_trefi, program_args.runtime_limit);
//    TraditionalHammerer::n_sided_hammer_experiment(memory, program_args.acts_per_trefi);
    TraditionalHammerer::n_sided_hammer_experiment_frequencies(memory);
  } else {
    Logger::log_error("Invalid combination of program control-flow arguments given. "
                      "Note: Fuzzing is only supported with synchronized hammering.");
  }

  Logger::close();
  return EXIT_SUCCESS;
}

void handle_arg_generate_patterns(int num_activations, const size_t probes_per_pattern) {
  // this parameter is defined in FuzzingParameterSet
  const size_t MAX_NUM_REFRESH_INTERVALS = 32;
  const size_t MAX_ACCESSES = num_activations*MAX_NUM_REFRESH_INTERVALS;
  void *rows_to_access = calloc(MAX_ACCESSES, sizeof(int));
  if (rows_to_access==nullptr) {
    Logger::log_error("Allocation of rows_to_access failed!");
    exit(EXIT_FAILURE);
  }
  FuzzyHammerer::generate_pattern_for_ARM(num_activations, static_cast<int *>(rows_to_access), static_cast<int>(MAX_ACCESSES), probes_per_pattern);
  exit(EXIT_SUCCESS);
}

void handle_args(int argc, char **argv) {
  // An option is specified by four things:
  //    (1) the name of the option,
  //    (2) the strings that activate the option (flags),
  //    (3) the option's help message,
  //    (4) and the number of arguments the option expects.
  argagg::parser argparser{{
      {"help", {"-h", "--help"}, "shows this help message", 0},
      {"dimm-id", {"-d", "--dimm-id"}, "internal identifier of the currently inserted DIMM (default: 0)", 1},
      {"ranks", {"-r", "--ranks"}, "number of ranks on the DIMM, used to determine bank/rank/row functions, assumes Intel Coffe Lake CPU (default: None)", 1},

      {"fuzzing", {"-f", "--fuzzing"}, "perform a fuzzing run (default program mode)", 0},
      {"generate-patterns", {"-g", "--generate-patterns"}, "generates N patterns, but does not perform hammering; used by ARM port", 1},
      {"replay-patterns", {"-y", "--replay-patterns"}, "replays patterns given as comma-separated list of pattern IDs", 1},

      {"load-json", {"-j", "--load-json"}, "loads the specified JSON file generated in a previous fuzzer run, loads patterns given by --replay-patterns or determines the best ones", 1},

      // note that these two parameters don't require a value, their presence already equals a "true"
      {"sync", {"-s", "--sync"}, "synchronize with REFRESH while hammering (default: present)", 0},
      {"sweeping", {"-w", "--sweeping"}, "sweep the best pattern over a contig. memory area after fuzzing (default: absent)", 0},

      {"runtime-limit", {"-t", "--runtime-limit"}, "number of seconds to run the fuzzer before sweeping/terminating (default: 120)", 1},
      {"acts-per-ref", {"-a", "--acts-per-ref"}, "number of activations in a tREF interval, i.e., 7.8us (default: None)", 1},
      {"probes", {"-p", "--probes"}, "number of different DRAM locations to try each pattern on (default: NUM_BANKS/4)", 1},
    }};

  argagg::parser_results parsed_args;
  try {
    parsed_args = argparser.parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    exit(EXIT_FAILURE);
  }

  if (parsed_args["help"]) {
    std::cerr << argparser;
    exit(EXIT_SUCCESS);
  }

  /**
   * mandatory parameters
   */
  if (parsed_args.has_option("dimm-id")) {
    program_args.dimm_id = parsed_args["dimm-id"].as<int>(0);
    Logger::log_debug(format_string("Set --dimm-id: %ld", program_args.dimm_id));
  } else {
    Logger::log_error("Program argument '--dimm-id <integer>' is mandatory! Cannot continue.");
    exit(EXIT_FAILURE);
  }

  if (parsed_args.has_option("ranks")) {
    program_args.num_ranks = parsed_args["ranks"].as<int>(0);
    Logger::log_debug(format_string("Set --ranks=%d", program_args.num_ranks));
  } else {
    Logger::log_error("Program argument '--ranks <integer>' is mandatory! Cannot continue.");
    exit(EXIT_FAILURE);
  }

  /**
  * optional parameters
  */
  program_args.sweeping = parsed_args.has_option("sweeping") || program_args.sweeping;
  Logger::log_debug(format_string("Set --sweeping=%s", (program_args.sweeping ? "true" : "false")));

  program_args.runtime_limit = parsed_args["runtime-limit"].as<unsigned long>(program_args.runtime_limit);
  Logger::log_debug(format_string("Set --runtime_limit=%ld", program_args.runtime_limit));

  program_args.acts_per_trefi = parsed_args["acts-per-ref"].as<size_t>(program_args.acts_per_trefi);
  Logger::log_info(format_string("+++ %d", program_args.acts_per_trefi));
  program_args.fixed_acts_per_ref = (program_args.acts_per_trefi != 0);
  Logger::log_debug(format_string("Set --acts-per-ref=%d", program_args.acts_per_trefi));

  program_args.num_address_mappings_per_pattern = parsed_args["probes"].as<size_t>(program_args.num_address_mappings_per_pattern);
  Logger::log_debug(format_string("Set --probes=%d", program_args.num_address_mappings_per_pattern));

  /**
   * program modes
   */
  if (parsed_args.has_option("generate-patterns")) {
    auto num_activations = parsed_args["generate-patterns"].as<int>(84);
    // this must happen AFTER probes-per-pattern has been parsed
    // note: the following method call does not return anymore
    handle_arg_generate_patterns(num_activations, program_args.num_address_mappings_per_pattern);
  } else if (parsed_args.has_option("load-json")) {
    program_args.load_json_filename = parsed_args["load-json"].as<std::string>("");
    if (parsed_args.has_option("replay-patterns")) {
      auto vec_pattern_ids = parsed_args["replay-patterns"].as<argagg::csv<std::string>>();
      program_args.pattern_ids = std::unordered_set<std::string>(
          vec_pattern_ids.values.begin(),
          vec_pattern_ids.values.end());
    } else {
      program_args.pattern_ids = std::unordered_set<std::string>();
    }
  } else {
    program_args.do_fuzzing = parsed_args["fuzzing"].as<bool>(true);
    const bool default_sync = true;
    program_args.use_synchronization = parsed_args.has_option("sync") || default_sync;
  }
}
