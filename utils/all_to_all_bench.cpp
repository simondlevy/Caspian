#include "network.hpp"
#include "simulator.hpp"
#include "ucaspian.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>

using namespace caspian;

void run_test(Backend *sim, int inputs, int runs, int seed, int runtime, int input_time, int conn_p, bool use_delay, bool print_outputs)
{
    auto rand_start = std::chrono::system_clock::now();

    const int max_weight = 127;
    const int threshold = 100;
    const int max_delay = 15;
    std::mt19937 gen{static_cast<uint64_t>(seed)};
    std::normal_distribution<> nd{0,1};
    std::uniform_real_distribution<double> conn_dist(0.0,1.0); 
    //std::uniform_int_distribution<> ud(0, max_delay);
    
    double conn = static_cast<double>(conn_p) / 100.0;

    int n_neurons = inputs;
    uint64_t accumulations = 0;
    uint64_t input_fire_cnt = 0;
    uint64_t active_cycles = 0;

    std::vector<std::chrono::duration<double>> sim_times;
    std::vector<long long> output_counts;

    Network net(n_neurons);

    // Generate the network
    for(int i = 0; i < inputs; ++i)
    {
        int dd = std::round(nd(gen) * (max_delay/2));
        int dly = std::max(0, std::min(max_delay, dd));

        if(!use_delay) dly = 0;

        net.add_neuron(i, threshold, -1, dly);
        net.set_input(i, i);
        net.set_output(i, i);
    }

    for(int i = 0; i < inputs; ++i)
    {
        for(int j = 0; j < inputs; ++j)
        {
            if(j == i) continue;

            // check if connected
            if(conn_dist(gen) > conn) continue;

            int wd = std::round(nd(gen) * (threshold/2));
            int w = std::max(-max_weight, std::min(max_weight, wd));

            // NOTE: No synaptic delay for now!
            //int dd = std::round(nd(gen) * 15);
            //int dly = std::max(0, std::min(max_delay, dd));
            //int dly = ud(gen);
            int dly = 0;

            net.add_synapse(i, j, w, dly);
        }
    }
    

    // Configure the simulator with the new network
    auto cfg_start = std::chrono::system_clock::now();

    sim->configure(&net);

    for(int i = 0; i < inputs; i++)
    {
        sim->track_timing(i);
    }

    auto cfg_end = std::chrono::system_clock::now();

    std::chrono::duration<double, std::micro> rnd_duration = (cfg_start - rand_start);
    std::chrono::duration<double, std::micro> cfg_duration = (cfg_end - cfg_start);

    printf("Seed: %d | Neurons: %zu Synapses: %zu | Cycles: %d | Input Duration: %d\n", 
            seed, net.num_neurons(), net.num_synapses(), runtime, input_time);
    printf("Random Net: %lf us\n", rnd_duration.count()); 
    printf("Configure : %lf us\n", cfg_duration.count());

    for(int r = 0; r < runs; ++r)
    {
        std::uniform_int_distribution<> dis(0, 100);

        auto sim_start = std::chrono::system_clock::now();

        // Queue up inputs
        for(int i = 0; i < inputs; ++i)
        {
            int frate = dis(gen);

            if(frate == 0) continue;

            for(int j = 0; j < input_time; ++j)
            {
                if(j % frate == 0)
                {
                    sim->apply_input(i, 255, j);
                    input_fire_cnt++;
                }
            }
        }

        // Simulate with specified time
        sim->simulate(runtime);

        int cnts = 0;
        for(int i = 0; i < inputs; ++i) cnts += sim->get_output_count(i);
        output_counts.push_back(cnts);

        auto sim_end = std::chrono::system_clock::now();

        accumulations += sim->get_metric("accumulate_count");
        active_cycles += sim->get_metric("active_clock_cycles");

        std::chrono::duration<double> sim_time = sim_end - sim_start;
        printf("Simulate %4d: %lf s\n", r, sim_time.count());
        sim_times.push_back(sim_time);

        if(print_outputs)
        {
            for(int i = 0; i < inputs; i++)
            {
                printf("%3d (%3d):", i, sim->get_output_count(i));
                auto vec = sim->get_output_values(i);
                for(auto v : vec) printf(" %u", v);
                printf("\n");
            }
        }

        sim->clear_activity();
    }

    std::sort(sim_times.begin(), sim_times.end());

    double avg = 0;
    for(auto const &t : sim_times) avg += t.count();
    avg /= sim_times.size();

    long long ocnts = std::accumulate(output_counts.begin(), output_counts.end(), 0);

    double avg_input_fires = static_cast<double>(input_fire_cnt) / static_cast<double>(runs);
    double avg_accum = static_cast<double>(accumulations) / static_cast<double>(runs);

    printf("\n");
    printf("---[Metrics]------------------------\n");
    printf("Average Simulate (s)     : %9.7f\n", avg);
    printf("Median Simulate  (s)     : %9.7f\n", sim_times[sim_times.size()/2].count());
    printf("Input Spikes             : %lf\n", avg_input_fires);
    printf("Output Spikes            : %lld\n", ocnts);
    printf("Accumulations            : %lu\n", accumulations);
    printf("Accumulations/second     : %.1f\n", avg_accum / avg);
    printf("Accumulations/step       : %.1f\n", avg_accum / runtime);
    printf("Effective Speed (KHz)    : %.4f\n", (static_cast<double>(runtime) / avg) / (1000) );

    if(active_cycles != 0)
    {
        // This is dependent on the actual clock speed of the dev board.
        const double clk_speed = 25000000;
        //const double clk_speed = 150000000; // previously 25000000
        double adj_time = (static_cast<double>(active_cycles) / clk_speed) / static_cast<double>(runs);
        printf("---[FPGA Metrics]-------------------\n");
        printf("Active Clock Cycles      : %lu\n", active_cycles);
        printf("Adj Runtime (s)          : %9.7f\n", adj_time);
        printf("Adj Accumulations/second : %.1f\n", avg_accum / adj_time);
        printf("Adj Effective Speed (KHz): %.4f\n", (runtime / adj_time) / (1000) );
    }
}

int main(int argc, char **argv)
{
    if(argc < 6)
    {
        printf("Usage: %s backend inputs n_runs runtime seed (delay: Y|N) (print_outputs: Y|N) (input_time) (percent connectivity)\n", argv[0]);
        return -1;
    }

    std::string backend = argv[1];
    int inputs = atoi(argv[2]);
    int runs = atoi(argv[3]);
    int rt = atoi(argv[4]);
    int seed = atoi(argv[5]);
    bool use_delay = false;
    bool print_outputs = false;
    int input_time = rt;
    int conn_p = 100;

    if(argc > 6)
    {
        if(argv[6][0] == 'Y')
        {
            use_delay = true;
            printf("Using axonal delay\n");
        }
    }

    if(argc > 7)
    {
        if(argv[7][0] == 'Y')
        {
            print_outputs = true;
        }
    }

    if(argc > 8)
    {
        input_time = atoi(argv[8]);
    }

    if(argc > 9)
    {
        conn_p = atoi(argv[9]);

        if(conn_p == 0) 
        {
            printf("Connectivity percentage must be greater an integer greater than 0!\n");
            return -1;
        }
    }

    std::unique_ptr<Backend> sim;

    if(backend == "sim")
    {
        printf("Using Simulator backend\n");
        sim = std::make_unique<Simulator>();
    }
    else if(backend == "sim-debug")
    {
        printf("Using Simulator backend\n");
        sim = std::make_unique<Simulator>(true);
    }
#ifdef WITH_USB
    else if(backend == "ucaspian")
    {
        printf("Using uCaspian backend\n");
        sim = std::make_unique<UsbCaspian>(false);
    }
    else if(backend == "ucaspian-debug")
    {
        printf("Using uCaspian backend\n");
        sim = std::make_unique<UsbCaspian>(true);
    }
#endif
#ifdef WITH_VERILATOR
    else if(backend == "verilator")
    {
        printf("Using uCaspian Verilator backend\n");
        sim = std::make_unique<VerilatorCaspian>(false);
    }
    else if(backend == "verilator-log")
    {
        printf("Using uCaspian Verilator backend - debug => a2a.fst\n");
        sim = std::make_unique<VerilatorCaspian>(true, "a2a.fst");
    }
#endif
    else
    {
#ifdef WITH_VERILATOR
        printf("Backend options: sim, sim-debug, ucaspian, ucaspian-debug, verilator, verilator-log\n");
#else
        printf("Backend options: sim, sim-debug, ucaspian, ucaspian-debug\n");
#endif
        return 0;
    }

    try {
        run_test(sim.get(), inputs, runs, seed, rt, input_time, conn_p, use_delay, print_outputs);
    }
    catch(...)
    {
        printf("There was an error completing the test.\n");
    }

    return 0;
}

/* vim: set shiftwidth=4 tabstop=4 softtabstop=4 expandtab: */
