#pragma once

#include <map>

#include "framework.hpp"
#include "backend.hpp"
#include "network.hpp"
#include "network_conversion.hpp"
#include "nlohmann/json.hpp"

namespace caspian
{
    using std::string;
    using std::vector;
    using neuro::Spike;
    using nlohmann::json;

    class Processor : public neuro::Processor
    {
    public:
        Processor(const json &j);
        ~Processor();

        // TODO: unique_ptr, shared_ptr, or raw pointer?
        bool load_network(neuro::Network* n, int network_id = 0);
        bool load_networks(vector<neuro::Network*>& n);

        /* Apply spike(s) to a network */
        void apply_spike(const Spike& s,
                         bool normalized = true,
                         int network_id = 0);
        void apply_spike(const Spike& s,
                         const vector<int>& network_ids,
                         bool normalized = true);

        /* Apply spike(s) to multiple networks */
        void apply_spikes(const vector<Spike>& spikes,
                          bool normalized = true,
                          int network_id = 0);
        void apply_spikes(const vector<Spike>& spikes,
                          const vector<int>& network_ids,
                          bool normalized = true);

        /* Run the network for the desired time with queued input(s) */
        void run(double duration, int network_id = 0);
        void run(double duration, const vector<int>& network_ids);

        /* Get processor time based on specified network */
        double get_time(int network_id = 0);

        /* Output tracker */
        void track_aftertime(int output_id, double aftertime, int network_id = 0);
        void track_output(int output_id, bool track = true, int network_id = 0);
        bool track_output_events(int output_id, bool track = true, int network_id = 0);

        /* Access output spike data */
        double output_last_fire(int output_id, int network_id = 0);
        int output_count(int output_id, int network_id = 0);
        vector<double> output_vector(int output_id, int network_id = 0);

        vector <double> output_last_fires(int network_id = 0);
        vector <int> output_counts(int network_id = 0);
        vector < vector <double> > output_vectors (int network_id = 0);

        /* Access all spike data -- added by Katie / JSP */
        bool track_neuron_events(uint32_t node_id, bool track = true, int network_id = 0);
        vector <int> neuron_counts(int network_id = 0);
        vector <double> neuron_last_fires(int network_id = 0);
        vector < vector <double> > neuron_vectors(int network_id = 0);
        vector < double > neuron_charges(int network_id = 0);
        void synapse_weights (vector <uint32_t> &pres,
                                  vector <uint32_t> &posts,
                                  vector <double> &vals,
                                  int network_id = 0);

        /* New processor functions -- added by Aaron */
        long long total_neuron_counts(int network_id = 0);
        long long total_neuron_accumulates(int network_id = 0);

        /* Remove the network from the processor */
        void clear(int network_id = 0);

        /* Remove state, keep network loaded */
        void clear_activity(int network_id = 0);

        neuro::PropertyPack get_network_properties() const;
        nlohmann::json get_processor_properties() const;
        nlohmann::json get_params() const;
        std::string get_name() const;

        /* Added for TENNLab viz */
        void track_spikes();
        void get_spike_counts(nlohmann::json& data);
        void get_spike_raster(nlohmann::json& data);


        /*** ADDED METHODS ***/
        caspian::Network* get_internal_network(int network_id = 0) const;
        json get_configuration() const;
        caspian::Backend* get_backend() const;

    protected:
        caspian::Backend* dev;

        json jconfig;
        json saved_params;
        neuro::PropertyPack properties;

        /* for single network sim */
        bool multi_net_sim = false;
        vector<neuro::Network*> api_nets;
        vector<caspian::Network*> internal_nets;

        void check_is_loaded(const int network_id);
    };

}

