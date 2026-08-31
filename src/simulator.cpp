#include <iostream>
#include <chrono>
#include <algorithm>

#include "simulator.hpp"
#include "network.hpp"
#include "constants.hpp"

namespace caspian
{
    using constants::delay_bucket;

    template <typename T>
    static inline T clamp(T value, T min_value, T max_value) noexcept
    {
        //return std::max(min_value, std::min(max_value, value));
        return (value > max_value) ? max_value : ((value < min_value) ? min_value : value);
    }

    void Simulator::refresh_neuron(Neuron *n) noexcept
    {
        int32_t imm = n->charge;

        // check and apply leak
        //   This approximates 2^(-t/tau) exponential leak
        //   only uses integer multiplication, addition/subtraction, and bit shifts
        //   along with a look up table with tau number of entries
        if(n->leak >= 0 && net_time > n->last_event)
        {
            int t = net_time - n->last_event;
            int shamt = t >> n->leak;
            int t_masked = t & ((1 << n->leak)-1);
            
            imm = (imm > 0) ? imm : -imm;

            if(t_masked != 0)
            {
                int comp_idx = ((1 << n->leak) - t_masked) 
                               * (1 << (constants::MAX_LEAK - n->leak));

                imm = (imm * constants::LEAK_COMP[comp_idx]) >> constants::COMP_BITS;
            }

            imm >>= shamt;
            imm = (n->charge > 0) ? imm : -imm;
        }

        // update last_event time
        n->last_event = net_time;

        // clamp charge between [min, max]
        n->charge = clamp(imm, constants::MIN_CHARGE, constants::MAX_CHARGE);
    }

    void Simulator::process_fire(const InputFireEvent &e)
    {
        for(Network *n : nets)
        {
            Neuron &to = n->get_neuron(n->get_input(e.id));

            // refresh the state of the neuron
            if(to.last_event != net_time)
                refresh_neuron(&to);

            // accumulate charge
            to.charge += e.weight;

            if(m_debug)
                printf("[t=%3lu] Neuron %2d charge: %4d after accumulating %4d\n",net_time, to.id, to.charge, e.weight);

            // increment accumulations count
            metric_accumulates++;

            // check threshold
            if(to.charge > to.threshold && !to.tcheck)
            {
                // add to list of elements to check
                thresh_check.emplace_back(&to);
                to.tcheck = true;
            }
        }
    }

    void Simulator::process_fire(const FireEvent &e) noexcept
    {
        if(e.neuron->last_event != net_time)
            refresh_neuron(e.neuron);

        // accumulate charge
        e.neuron->charge += e.syn->weight;

        if(m_debug)
            printf("[t=%3lu] Neuron %2d charge: %4d after accumulating %4d\n",net_time, e.neuron->id, e.neuron->charge, e.syn->weight);

        // increment accumulations count
        metric_accumulates++;

        // set synapse last fired
        //e.syn->last_fire = net_time;

        // check threshold
        if(e.neuron->charge > e.neuron->threshold && !e.neuron->tcheck)
        {
            // add to list of elements to check
            thresh_check.emplace_back(e.neuron);
            e.neuron->tcheck = true;
        }
    }

    void Simulator::threshold_check(Neuron *n) noexcept
    {
        // reset tcheck status
        n->tcheck = false;

        if(n->charge > n->threshold)
        {
            // increment count of fires
            metric_fires++;

            if(m_debug)
                printf("[t=%4lu] > FIRE %3d charge: %6d",net_time, n->id, n->charge);

            // optionally, collect every spike
            if(collect_all) 
            {
                all_spikes.back().push_back(n->id);
                all_spike_cnts[n->id]++;
            }

            // reset charge after firing (soft reset => charge - threshold, hard reset => 0)
            n->charge = (soft_reset) ? n->charge - n->threshold : 0;

            // todo: for soft_reset, if charge is still > threshold, schedule null event for t+1?

            // update last fired
            //n->last_fire = net_time;

            // create a fire event for each output of the neuron
            for(std::pair<Neuron*, Synapse*> & opair : n->outputs)
            {
                Synapse *syn = opair.second;

                // schedule the event based on the current time plus any synaptic or axonal delay
                uint64_t fire_idx = delay_bucket(net_time + syn->delay + n->delay, dly_mask);

                // add the fire event
                fires[fire_idx].emplace_back(syn, opair.first);
            }

            // monitor outputs
            //   --- output fires currently do *not* have axonal delay
            if(n->output_id >= 0)
            {
                // time since start of simulation call
                int64_t time_diff = net_time - run_start_time; 
                // is the current time after the specified starting time?
                bool after_start = (time_diff >= monitor_aftertime[n->output_id]);

                // check monitor times
                if(after_start)
                {
                    int tag = (multi_net_sim) ? n->tag : 0;
                    output_logs[tag].add_fire(n->output_id, net_time - run_start_time, monitor_precise[n->output_id]);
                    if(m_debug)
                        printf(" + output at %4lu",net_time - run_start_time);
                }
            }
            
            if(m_debug)
                printf("\n");
        }
    }

    void Simulator::do_cycle()
    {
        // add next list to all_spikes; might be empty if there are no fires
        all_spikes.push_back({});

        // check thresholds after all fires are processed for the timestep
        for(size_t i = 0; i < thresh_check.size(); ++i)
        {
            threshold_check(thresh_check[i]);
        }

        // clear processed neurons all at once
        thresh_check.clear();

        // process input fires
        while(!input_fires.empty() && input_fires.back().time == net_time)
        {
            process_fire(input_fires.back());
            input_fires.pop_back();
        }

        // determine bucket index => net_time % n_buckets
        size_t f_idx = delay_bucket(net_time, dly_mask);

        // process fire events in fire queue
        for(size_t i = 0; i < fires[f_idx].size(); ++i)
        {
            process_fire(fires[f_idx][i]);
        }

        // clear processed events all at once
        fires[f_idx].clear();
    }

    bool Simulator::configure(Network *n)
    {
        // clear all state variables inside simulation
        net_time = 0;
        input_map.clear();
        input_fires.clear();
        thresh_check.clear();

        // clear fire tracking
        monitor_aftertime.clear();
        monitor_precise.clear();
        output_logs.clear();
        all_spikes.clear();
        all_spike_cnts.clear();

        // clear internal fires
        for(auto &&f : fires)
            f.clear();

        // assign the network pointer
        net = n;
        nets.clear();
        nets.push_back(n);

        // extract meaningful configuration if network is not null
        if(n != nullptr)
        {
            uint32_t thresh_bits = 0;
            uint32_t max_thresh = net->max_thresh + 1;
            while(max_thresh >>= 1)
                thresh_bits++;

            // neuron soft reset
            soft_reset = net->soft_reset;

            // set up input mapping
            input_map.resize(net->num_inputs());
            for(size_t i = 0; i < net->num_inputs(); i++)
                input_map[i] = net->get_input(i);

            // set up output monitoring
            monitor_aftertime.resize(net->num_outputs(), -1);
            monitor_precise.resize(net->num_outputs(), false);
            output_logs.emplace_back(net->num_outputs());

            // Get maxmimum delay from network & allocate enough schedule slots in circular buffer
            int total_max_delay = net->max_axon_delay + net->max_syn_delay;
            max_delay = constants::next_pow_of_2(total_max_delay+1)-1;
            dly_mask = max_delay;
            fires.resize(max_delay+1);
        }

        return true;
    }

    bool Simulator::configure_multi(std::vector<Network*>& networks)
    {
        // if we can't configure the first... there are problems
        if(!configure(networks[0]))
        {
            // TODO?
            return false;
        }

        nets = networks;
        multi_net_sim = true;

        int tag = 0;

        for(Network *n : nets)
        {
            // Check to make sure everything makes sense
            if(n->num_inputs() != net->num_inputs()) return false;
            if(n->num_outputs() != net->num_outputs()) return false;

            // assign a tag to the outputs for each network to correspond with internal network id
            for(int outp = 0; outp < int(n->num_outputs()); outp++)
                n->get_neuron(n->get_output(outp)).tag = tag;

            tag++;
        }

        while(output_logs.size() < nets.size())
        {
            output_logs.emplace_back(net->num_outputs());
        }

        return true;
    }

    void Simulator::apply_input(int input_id, int16_t w, uint64_t t)
    {
        // note: adding +1 time for HW
        input_fires.emplace_back(input_id, w, net_time + t);
    }

    bool Simulator::simulate(uint64_t steps)
    {
        uint64_t end_time;

        // can't simulate if no network is configured
        if(net == nullptr)
            return false;

        // sort the inputs prior to starting simulation
        std::sort(input_fires.begin(), input_fires.end(), std::greater<InputFireEvent>());

        // clear fire tracking information
        for(auto &m : output_logs) m.clear();

        run_start_time = net->get_time();
        end_time = run_start_time + steps;

        all_spikes.clear();
        all_spike_cnts.clear();

        // ok, not a strictly event-based system for now
        for(net_time = run_start_time; net_time < end_time; ++net_time)
            do_cycle();

        // save updated time to the network
        for(Network *n : nets)
            n->set_time(end_time);

        metric_timesteps += steps;

        return true;
    }

    bool Simulator::update()
    {
        if(net == nullptr)
            return false;

        for(auto &&elm : net->elements)
            refresh_neuron(elm.second);

        return true;
    }

    uint64_t Simulator::get_time() const
    {
        return net_time;
    }

    Network* Simulator::pull_network(uint32_t idx) const
    {
        if(idx >= nets.size()) 
            throw std::out_of_range("[pull_network] network index is greater than the loaded networks");
        return nets[idx];
    }

    double Simulator::get_metric(const std::string &metric)
    {
        return get_metric_uint(metric);
    }

    uint64_t Simulator::get_metric_uint(const std::string &metric)
    {
        uint64_t m = 0;

        if(metric == "fire_count")
        {
            m = metric_fires;
            metric_fires = 0;
        }
        else if(metric == "accumulate_count")
        {
            m = metric_accumulates;
            metric_accumulates = 0;
        }
        else if(metric == "total_timesteps")
        {
            m = metric_timesteps;
            metric_timesteps = 0;
        }
        else if(metric == "active_clock_cycles")
        {
            m = 0;
        }
        else
        {
            std::cerr << "Specified device metric " << metric << " is not implemented\n"; 
        }

        return m;
    }

    void Simulator::reset()
    {
        net_time = 0;
        input_fires.clear();
        thresh_check.clear();
        all_spikes.clear();
        all_spike_cnts.clear();

        for(Network *n : nets)
            n->reset();

        // clear fire tracking information
        for(auto &a : monitor_aftertime) a = -1;
        for(auto &m : output_logs) m.clear();

        monitor_precise.clear();
        monitor_precise.resize(net->num_outputs(), false);

        for(auto &&f : fires)
            f.clear();
    }

    void Simulator::clear_activity()
    {
        net_time = 0;
        input_fires.clear();
        thresh_check.clear();
        all_spikes.clear();
        all_spike_cnts.clear();

        for(Network *n : nets)
            n->clear_activity();

        // clear fire tracking information
        for(auto &m : output_logs) m.clear();

        for(auto &f : fires) f.clear();
    }

    bool Simulator::track_aftertime(uint32_t output_id, uint64_t aftertime)
    {
        if(output_id >= monitor_aftertime.size()) return false;
        monitor_aftertime[output_id] = aftertime;
        return true;
    }
    
    bool Simulator::track_timing(uint32_t output_id, bool do_tracking)
    {
        if(output_id >= monitor_precise.size()) return false;
        monitor_precise[output_id] = do_tracking;
        return true;
    }

    int Simulator::get_output_count(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].fire_counts.size()) return -1;
        return output_logs[network_id].fire_counts[output_id];
    }

    int Simulator::get_last_output_time(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].last_fire_times.size()) return -1;
        return output_logs[network_id].last_fire_times[output_id];
    }

    std::vector<uint32_t> Simulator::get_output_values(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].recorded_fires.size()) 
            return std::vector<uint32_t>();

        return output_logs[network_id].recorded_fires[output_id];
    }

    void Simulator::set_debug(bool debug)
    {
        m_debug = debug;
    }

    void Simulator::collect_all_spikes(bool collect)
    {
        collect_all = collect;
    }

    std::vector<std::vector<uint32_t>> Simulator::get_all_spikes()
    {
        return all_spikes;
    }

    Simulator::UIntMap Simulator::get_all_spike_cnts()
    {
        return all_spike_cnts;
    }

    Simulator::Simulator(bool debug)
    {
        m_debug = debug;
    }
}

/* vim: set shiftwidth=4 tabstop=4 softtabstop=4 expandtab: */
