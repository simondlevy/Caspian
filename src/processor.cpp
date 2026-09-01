#include "backend.hpp"
#include "simulator.hpp"
#include "constants.hpp"
#include "ucaspian.hpp"
#include "processor.hpp"
#include "utils/json_helpers.hpp"
using json = nlohmann::json;

static json specs = {
    { "Backend",            "S" },
    { "Debug",              "B" },
    { "Allow_Lazy",         "B" },
    { "Verilator",          "J" },
    { "Min_Threshold",      "I" },
    { "Max_Threshold",      "I" },
    { "Leak_Enable",        "B" },
    { "Min_Leak",           "I" },
    { "Max_Leak",           "I" },
    { "Min_Weight",         "I" },
    { "Max_Weight",         "I" },
    { "Min_Axon_Delay",     "I" },
    { "Max_Axon_Delay",     "I" },
    { "Min_Synapse_Delay",  "I" },
    { "Max_Synapse_Delay",  "I" }
};

namespace caspian
{
    using std::vector;
    using std::string;
    using neuro::Spike;
    using neuro::Property;

    Processor::Processor(const json& j)
    {
        saved_params = j;

        // Default configuration
        jconfig = {
            { "Backend",                "Event_Simulator" },
            { "Debug",                  false },
            { "Allow_Lazy",             false },
            { "Verilator",              {{"Trace_File", ""}}},
            { "Leak_Enable",            true },
            { "Min_Leak",               0 },
            { "Max_Leak",               constants::MAX_LEAK },
            { "Min_Threshold",          constants::MIN_THRESHOLD },
            { "Max_Threshold",          constants::MAX_THRESHOLD },
            { "Min_Weight",             constants::MIN_WEIGHT },
            { "Max_Weight",             constants::MAX_WEIGHT },
            { "Min_Axon_Delay",         constants::MIN_AXON_DELAY },
            { "Max_Axon_Delay",         constants::MAX_AXON_DELAY },
            { "Min_Synapse_Delay",      constants::MIN_DELAY },
            { "Max_Synapse_Delay",      constants::MAX_DELAY }
        };

        // Apply updates from provided configuration
        if(!j.empty())
        {
            jconfig.update(j);
        }

        bool debug = jconfig["Debug"];

        if(jconfig["Backend"] == "Event_Simulator")
        {
            dev = new Simulator(debug);
        }
#ifdef WITH_USB
        else if(jconfig["Backend"] == "uCaspian_USB")
        {
            // Synaptic delay is not supported on this platform
            jconfig["Min_Synapse_Delay"] = 0;
            jconfig["Max_Synapse_Delay"] = 0;
            jconfig["Leak_Enable"] = false; // temporary until I support this

            printf("Open uCaspian device\n");
            dev = new UsbCaspian(debug);
        }
#endif
#ifdef WITH_VERILATOR
        else if(jconfig["Backend"] == "uCaspian_Verilator")
        {
            // Synaptic delay is not supported on this platform
            jconfig["Min_Synapse_Delay"] = 0;
            jconfig["Max_Synapse_Delay"] = 0;
            jconfig["Leak_Enable"] = false; // temporary until I support this

            string trace_file = "";

            if(jconfig.contains("Verilator") && jconfig["Verilator"].contains("Trace_File"))
            {
                trace_file = jconfig["Verilator"]["Trace_File"];
            }

            if(debug) printf("Open uCaspian Verilator");
            if(debug) printf(" (trace: %s)\n", trace_file.c_str());

            dev = new VerilatorCaspian(debug, trace_file);
        }
#endif
        else
        {
            throw std::runtime_error("Selected backend '" + jconfig["Backend"].get<string>() + "' is not supported.");
        }

        // Check the configuration
        std::string json_chk = neuro::Parameter_Check_Json(jconfig, specs);

        // VALIDATE the settings
        // TODO

        // Cannot continue with an invalid configuration
        if(!json_chk.empty())
            throw std::runtime_error(json_chk);

        if(!jconfig["Leak_Enable"].get<bool>())
        {
            jconfig["Min_Leak"] = -1;
            jconfig["Max_Leak"] = -1;
        }

        // Add neuron parameters
        properties.add_node_property("Threshold",
                jconfig["Min_Threshold"],
                jconfig["Max_Threshold"],
                neuro::Property::Type::INTEGER,
                1);
        properties.add_node_property("Leak",
                jconfig["Min_Leak"],
                jconfig["Max_Leak"],
                neuro::Property::Type::INTEGER,
                1);
        properties.add_node_property("Delay",
                jconfig["Min_Axon_Delay"],
                jconfig["Max_Axon_Delay"],
                neuro::Property::Type::INTEGER,
                1);

        // Add synapse parameters
        properties.add_edge_property("Weight",
                jconfig["Min_Weight"],
                jconfig["Max_Weight"],
                neuro::Property::Type::INTEGER,
                1);
        properties.add_edge_property("Delay",
                jconfig["Min_Synapse_Delay"],
                jconfig["Max_Synapse_Delay"],
                neuro::Property::Type::INTEGER,
                1);
    }

    Processor::~Processor()
    {
        // delete our internal device pointer
        if(dev != nullptr)
            delete dev;

        for(size_t i = 0; i < internal_nets.size(); i++)
        {
            delete internal_nets[i];
        }
    }

    neuro::PropertyPack Processor::get_network_properties() const
    {
        return properties;
    }

    // ADDED BY KATIE
    json Processor::get_processor_properties() const {
        json j;
        j["input_scaling_value"] = 255;
        j["binary_input"] = true;
        j["spike_raster_info"] = true;
        j["plasticity"] = "none";
        j["threshold_inclusive"] = false;
        j["integration_delay"] = true;
        j["run_time_inclusive"] = false;
        return j;
    }

    json Processor::get_params() const {
        return saved_params;
    }

    string Processor::get_name() const {
      return "caspian";
    }

    bool Processor::load_network(neuro::Network *n, int /* network_id */)
    {
        multi_net_sim = false;
        api_nets.clear();

        for(size_t i = 0; i < internal_nets.size(); ++i)
        {
            delete internal_nets[i];
        }
        internal_nets.clear();

        // keep the pointer
        api_nets.push_back(n);

        // make a new shadow network
        Network *internal_net = new Network();

        // convert to internal representation & handle the error case
        if(!network_framework_to_internal(n, internal_net))
        {
            // get rid of internal represntation for bad network
            delete internal_net;
            internal_net = nullptr;

            // save error message
            //throw std::runtime_error("Error converting given network to internal representation");

            // return failure
            return false;
        }

        internal_nets.push_back(internal_net);

        // configure the device
        return dev->configure(internal_net);
    }

    bool Processor::load_networks(vector<neuro::Network*> &n)
    {
        bool convert_error = false;

        // Enable multi-network batch mode
        multi_net_sim = true;
        api_nets = n;

        // clear old networks
        for(size_t i = 0; i < internal_nets.size(); ++i)
        {
            delete internal_nets[i];
        }
        internal_nets.clear();

        // convert networks
        for(size_t i = 0; i < api_nets.size(); ++i)
        {
            Network *internal_net = new Network();

            if(!network_framework_to_internal(api_nets[i], internal_net))
            {
                delete internal_net;
                convert_error = true;
                break;
            }

            internal_nets.push_back(internal_net);
        }

        if(convert_error)
        {
            for(size_t i = 0; i < internal_nets.size(); ++i)
            {
                delete internal_nets[i];
            }
            internal_nets.clear();
            api_nets.clear();
            return false;
        }

        return dev->configure_multi(internal_nets);
    }

    void Processor::apply_spike(const Spike& s,
                                bool normalized,
                                int network_id)
    {
        check_is_loaded(network_id);

        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> apply_spike\n");

        int16_t int_val;

        if (normalized) {
            int_val = s.value * caspian::constants::MAX_DEVICE_INPUT;
        } else {
            int_val = s.value;
            if (int_val < 0 || int_val > caspian::constants::MAX_DEVICE_INPUT)
                throw std::runtime_error("[apply] Bad spike value: " +
                        std::to_string(s.value) + ": integer part must be >= 0 and <= " 
                    + std::to_string(caspian::constants::MAX_DEVICE_INPUT));
        }

        dev->apply_input(s.id, int_val, s.time);
    }

    void Processor::apply_spike(const Spike& s,
                                const vector<int>& network_ids,
                                bool normalized)
    {
        (void) s;
        (void) network_ids;
        (void) normalized;
        throw std::invalid_argument("Batch spike is not supported");
    }

    void Processor::apply_spikes(const vector<Spike>& spikes,
                                 bool normalized,
                                 int network_id)
    {
        // Error check? Not currently needed
        for(const Spike &s : spikes)
            apply_spike(s, normalized, network_id);
    }

    void Processor::apply_spikes(const vector<Spike>& spikes,
                                const vector<int>& network_ids,
                                bool normalized)
    {
        // Error check? Not currently needed
        for(const Spike &s : spikes)
            apply_spike(s, network_ids, normalized);
    }

    void Processor::run(double duration, int network_id)
    {
        check_is_loaded(network_id);

        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> run\n");

        dev->simulate(duration);
    }

    void Processor::run(double duration, const vector<int>& network_ids)
    {
        (void) duration;
        (void) network_ids;
        throw std::invalid_argument("Batch run is not supported");
    }

    double Processor::get_time(int network_id)
    {
        check_is_loaded(network_id);

        return dev->get_time();
    }

    void Processor::track_aftertime(int output_id, double aftertime, int network_id)
    {
        check_is_loaded(network_id);

        // time conversion
        uint64_t atime = aftertime;

        dev->track_aftertime(output_id, atime);
    }

    void Processor::track_output(int output_id, bool track, int network_id)
    {
        check_is_loaded(network_id);

        dev->track_timing(output_id, track);
    }

    // NOTE: Added by Katie
    bool Processor::track_output_events(int output_id, bool track, int network_id)
    {
        check_is_loaded(network_id);

        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> track_output_events\n");

        dev->track_timing(output_id, track);
        return true;
    }

    // NOTE: Added by Katie
    bool Processor::track_neuron_events(uint32_t node_id, bool track, int network_id) {
        (void) track;
        (void) node_id;
        check_is_loaded(network_id);
        dev->collect_all_spikes();
        return true;
    }

    double Processor::output_last_fire(int output_id, int network_id)
    {
        check_is_loaded(network_id);

        return static_cast<double>(dev->get_last_output_time(output_id, network_id));
    }

    int Processor::output_count(int output_id, int network_id)
    {
        check_is_loaded(network_id);
        return dev->get_output_count(output_id, network_id);
    }

    vector<double> Processor::output_vector(int output_id, int network_id)
    {
        check_is_loaded(network_id);

        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> output_vector\n");

        std::vector<uint32_t> i_times = dev->get_output_values(output_id, network_id);
        return std::vector<double>(i_times.begin(), i_times.end());
    }


    // NOTE: Added by Katie
    vector <double> Processor::output_last_fires(int network_id) {
        int i;
        vector <double> times;
        check_is_loaded(network_id);
        for (i = 0; i < api_nets[network_id]->num_outputs(); i++) {
            times.push_back(static_cast<double>(dev->get_last_output_time(i, network_id)));
        }
        return times;
    }

    // NOTE: Added by Katie
    vector <int> Processor::output_counts(int network_id) {
        int i;
        vector <int> counts;
        check_is_loaded(network_id);
        for (i = 0; i < api_nets[network_id]->num_outputs(); i++) {
            counts.push_back(dev->get_output_count(i, network_id));
        }
        return counts;
    }


    // NOTE: Added by Katie
    vector <vector <double> > Processor::output_vectors(int network_id) {
        int i;
        vector <vector <double> > ret;
        vector <double> x;
        check_is_loaded(network_id);
        for (i = 0; i < api_nets[network_id]->num_outputs(); i++) {
            x = output_vector(i, network_id);
            ret.push_back(x);
        }
        return ret;
    }

    // NOTE: Added by Katie
    vector <int> Processor::neuron_counts(int network_id) {
        check_is_loaded(network_id);
        auto sp_cnts = dev->get_all_spike_cnts();
        std::map <int, int> id_to_index;
        int i;
        std::vector<int> cnts;
        api_nets[network_id]->make_sorted_node_vector();
        auto snv = api_nets[network_id]->sorted_node_vector;
        cnts.resize(snv.size(), 0);
        for (i = 0; i < (int)snv.size(); i++) {
            id_to_index[snv[i]->id] = i;
        }
        for(auto const &s : sp_cnts)
        {
            cnts[id_to_index[s.first]] = s.second;
        }

        return cnts;
    }

    // NOTE: Added by Katie
    vector <double> Processor::neuron_last_fires(int network_id) {
        int i,j;
        std::vector <std::vector<uint32_t> > all_spikes;
        std::vector <double> last_times;
        api_nets[network_id]->make_sorted_node_vector();
        auto snv = api_nets[network_id]->sorted_node_vector;

        check_is_loaded(network_id);
        all_spikes = dev->get_all_spikes();
        last_times.resize(snv.size(), -1);
        for (i = 0; i < (int)all_spikes.size(); i++) {
            for (j = 0; j < (int)all_spikes[i].size(); j++) {
                last_times[all_spikes[i][j]] = i;
            }
        }
        return last_times;
    }

    // NOTE: Added by Katie
    vector <vector <double> > Processor::neuron_vectors(int network_id) {
        std::vector <std::vector<uint32_t> > all_spikes;
        std::vector <std::vector<double> > ret_all_spikes;
        int i, j;
        api_nets[network_id]->make_sorted_node_vector();
        auto snv = api_nets[network_id]->sorted_node_vector;
        check_is_loaded(network_id);

        all_spikes = dev->get_all_spikes();
        ret_all_spikes.resize(snv.size());
        for (i = 0; i < (int)all_spikes.size(); i++) {
            for (j = 0; j < (int)all_spikes[i].size(); j++) {
                ret_all_spikes[all_spikes[i][j]].push_back(i);
            }
        }
        return ret_all_spikes;
    }

    // NOTE: Added by JSP
    vector < double > Processor::neuron_charges(int network_id) {
        std::vector <double > rv;
        size_t i;
        Neuron *n;
        api_nets[network_id]->make_sorted_node_vector();
        auto snv = api_nets[network_id]->sorted_node_vector;

        check_is_loaded(network_id);

        for (i = 0; i < snv.size(); i++) {
          n = internal_nets[network_id]->get_neuron_ptr(snv[i]->id);
          if (n == NULL) {
            fprintf(stderr, "Internal caspian error.  Couldn't get neuron with id: %u\n",
               snv[i]->id);
            exit(1);
          }
          rv.push_back(n->charge);
        }
        return rv;

    }

    // NOTE: Added by Aaron to match API. Currently not implemented.
    long long Processor::total_neuron_counts(int network_id) {
        check_is_loaded(network_id);
        return -1;
    }

    // NOTE: Added by Aaron to match API. Currently not implemented.
    long long Processor::total_neuron_accumulates(int network_id) {
        check_is_loaded(network_id);
        return -1;
    }

    /* Removes the network */
    void Processor::clear(int network_id)
    {
        check_is_loaded(network_id);
        // TODO: network_id

        api_nets.clear();

        for(size_t i = 0; i < internal_nets.size(); i++)
        {
            delete internal_nets[i];
        }
        internal_nets.clear();

        dev->configure(nullptr);
        multi_net_sim = false;
    }

    /* Implemented by CDS -- Caspian doesn't support plasticity,
       so the vectors are all empty. */
    void Processor::synapse_weights(vector <uint32_t> &pres,
                                  vector <uint32_t> &posts,
                                  vector <double> &vals,
                                  int network_id) {
        (void) pres;
        (void) posts;
        (void) vals;
        (void) network_id;
        return;
    }

    /* Clears the state of a network */
    void Processor::clear_activity(int network_id)
    {
        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> clear_activity: dev=%p\n",
                dev);

        check_is_loaded(network_id);

        dev->clear_activity();
    }

    caspian::Network* Processor::get_internal_network(int network_id) const
    {
        if(network_id > int(internal_nets.size())-1)
            return nullptr;

        return internal_nets.at(network_id);
    }

    caspian::Backend* Processor::get_backend() const
    {
        return dev;
    }

    json Processor::get_configuration() const
    {
        return jconfig;
    }

    void Processor::track_spikes()
    {
        dev->collect_all_spikes();
    }

    void Processor::get_spike_counts(nlohmann::json& data)
    {
        // Not necessarily efficient
        auto sp_cnts = dev->get_all_spike_cnts();
        std::vector<int> cnts;
        std::vector<int> neurons;

        for(auto const &s : sp_cnts)
        {
            neurons.push_back(s.first);
            cnts.push_back(s.second);
        }

        data["Event Counts"] = cnts;
        data["Neuron Alias"] = neurons;
    }

    void Processor::get_spike_raster(nlohmann::json& data)
    {
        // TODO
        data["Event Raster"] = nlohmann::json::array();
        data["Neuron Alias"] = nlohmann::json::array();
    }

    void Processor::check_is_loaded(const int network_id)
    {
        return;
        if(network_id > int(internal_nets.size())-1)
            throw std::runtime_error("[apply] Specified network " +
                    std::to_string(network_id) + " is not loaded");
    }

}
