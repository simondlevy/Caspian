#include "network.hpp"
#include "simulator.hpp"
#include "ucaspian.hpp"
#include <memory>
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
//#include <fmt/format.h>
//#include <fmt/ostream.h>

using namespace caspian;

void generate_pass(Network *net, int width, int height, int delay = 0)
{
    auto idx = [width](int r, int c) {
        return r * width + c;
    };

    for(int row = 0; row < height; ++row)
    {
        for(int col = 0; col < width; ++col)
        {
            net->add_neuron(idx(row,col), 1, -1, delay);
            if(col != 0)
            {
                net->add_synapse(idx(row, col-1), idx(row,col), 127, 0);
            }

            if(col == 0)
            {
                net->set_input(idx(row, col), row);
            }
            else if(col == width-1)
            {
                net->set_output(idx(row, col), row);
            }
        }
    }
}

void run_test(Backend *sim, int w, int h, int runs, int runtime = 0, int ifires = 1, int adly = 0)
{
    auto net = std::make_unique<Network>(w*h);
    std::vector<std::chrono::duration<double>> sim_times;

    // Generate the pass network
    generate_pass(net.get(), w, h, adly);

    // Save the network to a file
    printf("Writing network to passnet.json\n");
    //fmt::print("Writing network to passnet.json\n");
    std::ofstream net_writer("passnet.json");
    net.get()->to_stream(net_writer);

    // Configure the simulator with the new network
    auto cfg_start = std::chrono::system_clock::now();
    sim->configure(net.get());

    for(size_t i = 0; i < net->num_outputs(); i++)
        sim->track_timing(i);

    auto cfg_end = std::chrono::system_clock::now();

    int cycles = (runtime == 0) ? 3*w + 2*h : runtime;
    printf("Width: %d Height: %d Cycles: %d\n", w, h, cycles);
    printf("Neurons: %zu Synapses: %zu\n", net->num_neurons(), net->num_synapses());
    printf("Configuration Time: %lf us\n", (cfg_end - cfg_start).count() / 1000.0);
    //fmt::print("Width: {} Height: {} Cycles: {}\n", w, h, cycles);
    //fmt::print("Neurons: {} Synapses: {}\n", net->num_neurons(), net->num_synapses());
    //fmt::print("Configuration Time: {} us\n", (cfg_end - cfg_start).count() / 1000.0);

    uint64_t accumulations = 0;
    uint64_t fires = 0;
    uint64_t outputs = 0;
    uint64_t active_cycles = 0;

    for(int r = 0; r < runs; ++r)
    {
        auto sim_start = std::chrono::system_clock::now();
        // Queue up inputs
        for(int f = 0; f < ifires; ++f)
        {
            for(int i = 0; i < h; ++i)
            {
                sim->apply_input(i, 255, f*h+i);
            }
        }

        // Simulate with sufficient time
        sim->simulate(cycles);
        auto sim_end = std::chrono::system_clock::now();

        std::chrono::duration<double> sim_time = sim_end - sim_start;
        printf("Simulate %4d: %lf s\n", r, sim_time.count());
        //fmt::print("Simulate {:4d}: {} s\n", r, sim_time.count());
        sim_times.push_back(sim_time);

        accumulations += sim->get_metric("accumulate_count");
        fires += sim->get_metric("fire_count");
        active_cycles += sim->get_metric("active_clock_cycles");

        for(int i = 0; i < h; ++i)
        {
            printf("Output %d (%d):", i, sim->get_output_count(i));
            //fmt::print("Output {} ({}):", i, sim->get_output_count(i));
            auto outs = sim->get_output_values(i);
            for(auto o : outs) printf(" %u", o);
            printf("\n");
            outputs += sim->get_output_count(i);
        }

        sim->clear_activity();
    }

    std::sort(sim_times.begin(), sim_times.end());

    double ttime = 0;
    for(auto const &t : sim_times) ttime += t.count();
    double avg = ttime / sim_times.size();

    double avg_accum = static_cast<double>(accumulations) / static_cast<double>(runs);

    printf("\n");
    printf("---[Metrics]------------------------\n");
    printf("Average Simulate (s)     : %9.7f\n", avg);
    printf("Median Simulate  (s)     : %9.7f\n", sim_times[sim_times.size()/2].count());
    printf("Spikes                   : %lu\n", fires);
    printf("Spikes/second            : %lf\n", static_cast<double>(fires) / ttime);
    printf("Output Spikes            : %lu\n", outputs);
    printf("Accumulations            : %lu\n", accumulations);
    printf("Accumulations/second     : %.1f\n", static_cast<double>(accumulations) / ttime);
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
    std::string backend;
    int w = 250;
    int h = 1;
    int runs = 1;
    int rt = 0;
    int ifires = 1;
    int dly = 0;

    if(argc < 5)
    {
        printf("Usage: %s backend width height n_runs (runtime) (fires) (delay)\n", argv[0]);
        //fmt::print("Usage: {} backend width height n_runs (runtime) (fires) (delay)\n", argv[0]);
        return -1;
    }

    if(argc >= 5)
    {
        backend = argv[1];
        w = atoi(argv[2]);
        h = atoi(argv[3]);
        runs = atoi(argv[4]);
    }

    if(argc >= 6)
    {
        rt = atoi(argv[5]);
    }

    if(argc >= 7)
    {
        ifires = atoi(argv[6]);
    }

    if(argc >= 8)
    {
        dly = atoi(argv[7]);
    }

    if(dly > 15)
    {
        printf("Delay may not be greater than 15! Given %d\n", dly);
        return -1;
    }

    std::unique_ptr<Backend> sim;

    if(backend == "sim")
    {
        printf("Using Simulator backend\n");
        sim = std::make_unique<Simulator>();
    }
    else if(backend == "debug")
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
        printf("Using uCaspian Verilator backend - debug => pass.fst\n");
        sim = std::make_unique<VerilatorCaspian>(true, "pass.fst");
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
        run_test(sim.get(), w, h, runs, rt, ifires, dly);
    }
    catch(...)
    {
        printf("There was an error completing the test.\n");
    }

    return 0;
}

/* vim: set shiftwidth=4 tabstop=4 softtabstop=4 expandtab: */
