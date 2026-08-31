#include "network.hpp"
#include "simulator.hpp"
#include "ucaspian.hpp"
#include "network_conversion.hpp"
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>
//#include <fmt/format.h>
//#include <fmt/ostream.h>

using namespace caspian;

void convert(const std::string &network_file)
{
    std::ifstream net_fstream(network_file);
    Network net;
    net.from_stream(net_fstream);

    std::stringstream ss_neurons;
    std::stringstream ss_synapses;

    ss_neurons << "Neuron neurons[] = {";
    ss_synapses << "Synapse synapses[] = {";

    // Generate configuration packets
    int syn_cnt = 0;
    unsigned int neuron_cnt = 0;
    for(auto &&elm : (net))
    {
        const Neuron *n = elm.second;

        // add neuron
        int n_syn_start = syn_cnt;
        int n_syn_cnt = n->outputs.size();
        bool output_en = (n->output_id >= 0);

        ss_neurons << "{" << n->id << "," << n->threshold << "," << std::to_string(n->delay) << "," << std::to_string(n->leak) << "," << output_en << "," << n_syn_start << "," << n_syn_cnt << "}";
        neuron_cnt++;
        if (neuron_cnt != net.num_neurons()) ss_neurons << ",";

        // add synapses
        for(const std::pair<Neuron*, Synapse*> &p : n->outputs)
        {
            ss_synapses << "{" << syn_cnt << "," << p.second->weight << "," << p.first->id << "}";
            syn_cnt++;
            if (static_cast<unsigned int>(syn_cnt) != net.num_synapses()) ss_synapses << ",";
        }
    }

    ss_neurons << "};";
    ss_synapses << "};";

    std::cout << "Paste the following snippet into the microcontroller program to configure this network:\n\n";
    std::cout << ss_neurons.str() << "\n\n";
    std::cout << ss_synapses.str() << "\n\n";
    std::cout << "ucaspian_config_network(dev, neurons, " << net.num_neurons() << ", synapses, " << net.num_synapses() << ");\n" << std::endl;
}

int main(int argc, char **argv)
{
    std::string filename;

    if(argc != 2)
    {
        printf("Usage: %s network_filename.json\n", argv[0]);
        //fmt::print("Usage: {} network_filename.json\n", argv[0]);
        return -1;
    }

    filename = argv[1];

    convert(filename);

    return 0;
}

/* vim: set shiftwidth=4 tabstop=4 softtabstop=4 expandtab: */
