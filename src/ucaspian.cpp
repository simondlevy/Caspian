#ifdef WITH_USB
#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "ucaspian.hpp"
#include "network.hpp"
#include "constants.hpp"

#define OUTPUT_FIRE (128)
#define CFG_ACK      (24)
#define CLR_ACK       (4)
#define METRIC_RESP   (2)
#define TIME_UPDATE   (1)

namespace caspian
{
    static const std::map<std::string, std::vector<uint8_t>> metric_addrs = {
        { "fire_count",          {1, 2, 3, 4} },
        { "accumulate_count",    {5, 6, 7, 8} },
        { "active_clock_cycles", {9, 10, 11, 12} },
        { "total_timesteps",     {} }
    };
    
    inline void make_input_fire(std::vector<uint8_t>& buf, uint8_t id, uint8_t val)
    {
        uint8_t op = (1 << 7) | id;
        buf.insert(buf.end(), {op, val});
    }

    inline void make_step(std::vector<uint8_t>& buf, uint8_t steps)
    {
        buf.insert(buf.end(), {1, steps});
    }

    inline void make_get_metric(std::vector<uint8_t>& buf, uint8_t addr)
    {
        buf.insert(buf.end(), {2, addr});
    }

    inline void make_clear_activity(std::vector<uint8_t>& buf)
    {
        buf.push_back(4);
    }

    inline void make_clear_config(std::vector<uint8_t>& buf)
    {
        buf.push_back(5);
    }

    inline void make_cfg_neuron(std::vector<uint8_t>& buf, uint8_t addr, uint8_t threshold, 
            uint8_t delay, int8_t leak, bool output, uint16_t syn_start, uint8_t syn_cnt)
    {
        uint8_t enc_leak = leak + 1;
        uint8_t out_flag = (output) ? (1 << 3) : 0;
        uint8_t dly_and_flg = ((delay & 0x0F) << 4) | out_flag | (enc_leak);
        uint8_t syn_0 = (syn_start >> 8) & 0x0F;
        uint8_t syn_1 = syn_start & 0xFF;

        buf.insert(buf.end(), {8, addr, threshold, dly_and_flg, syn_0, syn_1, syn_cnt});
    }

    inline void make_cfg_synapse(std::vector<uint8_t>& buf, uint16_t addr, int8_t weight, uint8_t target)
    {
        uint8_t addr_0 = (addr >> 8) & 0x0F;
        uint8_t addr_1 = addr & 0x00FF;

        buf.insert(buf.end(), {16, addr_0, addr_1, static_cast<uint8_t>(weight), target});
    }

    UsbCaspian::UsbCaspian(bool debug, std::string device)
    {
        printf("+++++++++++++++++++++++ UsbCaspian()\n");

        int ret;

        // Set up hardware state
        hw_state = std::make_unique<HardwareState>(debug);
        set_debug(debug);

        // TODO: interpret device string
        if(device != "verilator")
        {
            // create ftdi context struct (c library)
            if((ftdi = ftdi_new()) == 0)
            {
                throw std::runtime_error("Could not create libftdi context");
            }

            // attempt to open the device with the given vendor/device id
            // Mimas 2a19:1009
            //if((ret = ftdi_usb_open(ftdi, 0x2a19, 0x1009)) < 0)
            if((ret = ftdi_usb_open(ftdi, 0x0403, 0x6014)) < 0)
            {
                char err[1000] = "libftdi usb open error: ";
                const char *ftdi_err = ftdi_get_error_string(ftdi);
                strcat(err,ftdi_err);
                ftdi_free(ftdi);
                throw std::runtime_error(err);
            }

            // set latency timer
            // ftdi_set_latency_timer(ftdi, 1);
            // set latency timer
            if (ftdi_set_latency_timer(ftdi, 100) < 0)
            {
               fprintf(stderr,"Can't set latency timer: %s\n",ftdi_get_error_string(ftdi));
               exit(0);
            }

            // set baudrate
            ftdi_set_baudrate(ftdi, 3000000);
            printf("real baudrate used: %d\n", ftdi->baudrate);

            // Set Bitmode
            if (ftdi_set_bitmode(ftdi, 0xFF,BITMODE_RESET) < 0)
            {
               fprintf(stderr,"Can't set mode: %s\n",ftdi_get_error_string(ftdi));
               exit(0);
            }

            // Turn off flow control
            ftdi_setflowctrl(ftdi, 0);

            ////Bitmode Reset
            //ret = ftdi_set_bitmode(ftdi, 0xFF, BITMODE_RESET);
            //if (ret != 0) {
            //   ftdi_usb_close(ftdi);
            //   printf("unable to RESET bitmode of Tricorder\n");
            //   exit(0);
            //}

            ////Set FT 245 Synchronous FIFO mode
            //ret = ftdi_set_bitmode(ftdi, 0xFF, BITMODE_SYNCFF);
            //if (ret != 0) {
            //   ftdi_usb_close(ftdi);
            //   printf("unable to set synchronous FIFO mode of Tricorder\n");
            //   exit(0);
            //}

            // purge USB buffers on FT chip
            ftdi_tcioflush(ftdi);
        }
        else
        {
            ftdi = nullptr;
        }

        set_debug(true);
    }

    UsbCaspian::~UsbCaspian()
    {
        if(ftdi != nullptr) ftdi_free(ftdi);
    }

    int UsbCaspian::send_cmd(const std::vector<uint8_t> &buf)
    {
        return ftdi_write_data(ftdi, buf.data(), buf.size());
    }

    std::vector<uint8_t> UsbCaspian::rec_cmd(int max_size)
    {
        if(max_size > 8192) throw std::logic_error("Cannot get more than 8k in one command");

        uint8_t buf[8192];
        int bytes = ftdi_read_data(ftdi, buf, max_size);
        return std::vector<uint8_t>(buf, buf+bytes);
    }

    int HardwareState::parse_cmds_cond(const std::vector<uint8_t> &buf, std::function<bool(HardwareState*)> &cond_func)
    {
        if(m_debug)
            printf("Enter parse_cmds -- buf size: %zu\n",buf.size());
        size_t offset = 0;

        while(offset < buf.size())
        {
            int rem = buf.size() - offset;
            int inc = parse_cmd(&(buf[offset]), rem);
            offset += inc;
            if(inc == 0) break;
            if(cond_func(this)) break;
        }

        return offset;
    }

    int HardwareState::parse_cmds(const std::vector<uint8_t> &buf)
    {
        if(m_debug)
            printf("Enter parse_cmds -- buf size: %zu\n",buf.size());
        size_t offset = 0;

        while(offset < buf.size())
        {
            int rem = buf.size() - offset;
            int inc = parse_cmd(&(buf[offset]), rem);
            offset += inc;
            if(inc == 0) break;
        }

        return offset;
    }

    int HardwareState::parse_cmd(const uint8_t *buf, int rem)
    {
        int incr = 1;
        int addr;
        uint32_t id;
        uint64_t t = 0;
        int64_t time_diff;
        bool after_start;
        uint8_t metric_addr, metric_value;

        switch(buf[0])
        {
            case CFG_ACK:
                cfg_acks++;
                if(m_debug)
                    printf(" > Config Ack %u\n",cfg_acks);
                break;

            case CLR_ACK:
                clr_acks++;
                if(m_debug)
                    printf(" > Clear Ack %u\n",clr_acks);
                break;

            case METRIC_RESP:
                if(rem < 3)
                {
                    return 0;
                }
                if(m_debug)
                    printf(" > Metric Response\n");

                metric_addr = buf[1];
                metric_value = buf[2];
                rec_metrics.emplace_back(metric_addr, metric_value);
                incr = 3;
                break;

            case TIME_UPDATE:
                if(rem < 5)
                {
                    return 0;
                }

                t = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
                incr = 5;

                if(m_debug)
                    printf(" > Time Update: %lu\n",t);

                if(t - net_time > 255)
                    printf("Corrupted time %lu -> %lu -- %d %d %d %d %d %d\n", net_time, t, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

                // update time
                net_time = t;

                break;

            case OUTPUT_FIRE:

                if(rem < 2)
                {
                    return 0;
                }

                addr = buf[1];
                incr = 2;

                if(m_debug)
                    printf(" > Fire %d [t=%lu]\n", addr, net_time);

                if(!net->is_neuron(addr))
                {
                    printf("Corrupted fire %d\n",addr);
                    break;
                }

                id = net->get_neuron(addr).output_id;

                // add fire
                time_diff = net_time - run_start_time;
                after_start = (time_diff >= monitor_aftertime[id]);

                if(after_start)
                {
                    output_logs[0].add_fire(id, net_time - run_start_time, monitor_precise[id]);
                }
                
                break;

            default:
                // do nothing
                break;
        }
        
        return incr;
    }

    void UsbCaspian::apply_input(int input_id, int16_t w, uint64_t t)
    {
        printf("+++++++++++++++++++++++ apply_input()\n");

        input_fires.emplace_back(net->get_input(input_id), w, hw_state->net_time + t);
    }

    bool UsbCaspian::configure(Network *new_net)
    {
        printf("+++++++++++++++++++++++ configure()\n");

        std::vector<uint8_t> cfg_buf;
        int syn_cnt = 0;

        if(new_net == nullptr)
        {
            net = nullptr;
            return false;
        }
        else if(new_net->num_neurons() > 256 || new_net->num_synapses() > 4096)
        {
            std::cerr << "Network is too large with " <<
                std::to_string(new_net->num_neurons()) << 
                " neurons and " << std::to_string(new_net->num_synapses()) <<
                " synapses for the uCaspian device\n"; 

            net = nullptr;
            return false;
        }

        // check if inputs are id <= 127
        for(size_t i = 0; i < new_net->num_inputs(); i++)
        {
            uint32_t nid = new_net->get_input(i);
            if(nid > 127)
            {
                std::cerr << "Network input neurons must have an id <= 127 for uCaspian.\n"; 
                std::cerr << "Input " << std::to_string(i) << " is neuron with id="
                    << std::to_string(nid) << std::endl;
                net = nullptr;
                return false;
            }
        }

        net = new_net;
        hw_state->configure(net);

        // clear configuration
        make_clear_config(cfg_buf);
        if(m_debug)
            printf("Preparing to send clear config...");
        send_and_read(cfg_buf, [](HardwareState *hw){ return hw->clr_acks > 0; });
        if(m_debug)
            printf(" Clear ack'd\n");
        cfg_buf.clear();

        unsigned int elms_prog = 0;

        // Generate configuration packets
        for(auto &&elm : (*net))
        {
            const Neuron *n = elm.second;

            // add neuron
            int n_syn_start = syn_cnt;
            int n_syn_cnt = n->outputs.size();
            bool output_en = (n->output_id >= 0);
            make_cfg_neuron(cfg_buf, n->id, n->threshold, n->delay, n->leak,
                    output_en, n_syn_start, n_syn_cnt);
            elms_prog++;

            // add synapses
            for(const std::pair<Neuron*, Synapse*> &p : n->outputs)
            {
                make_cfg_synapse(cfg_buf, syn_cnt, p.second->weight, p.first->id);
                syn_cnt++;
                elms_prog++;
            }
        }

        if(elms_prog > 0)
        {
            std::cout << "Send config for " << std::to_string(elms_prog) << " elements with " 
                << std::to_string(cfg_buf.size()) << " bytes\n";
            send_and_read(cfg_buf, [=](HardwareState *hw){ return hw->cfg_acks >= elms_prog; });
        }

        return true;
    }

    bool UsbCaspian::configure_multi(std::vector<Network*>& /* networks */)
    {
        throw std::logic_error("Configure Multi is not implemented for uCaspian (yet...)");
        return false;
    }

    void UsbCaspian::clear_activity()
    {
        printf("+++++++++++++++++++++++ clear_activity()\n");

        std::vector<uint8_t> send_buf;
        make_clear_activity(send_buf);
        hw_state->clr_acks = 0;

        send_and_read(send_buf, [](HardwareState *hw){ return hw->clr_acks > 0; });

        hw_state->clear();
        input_fires.clear();
    }

    bool UsbCaspian::simulate(uint64_t steps)
    {
        printf("+++++++++++++++++++++++ simulate()\n");

        std::vector<uint8_t> send_buf;
        uint64_t end_time = hw_state->net_time + steps;
        uint64_t cur_time = hw_state->net_time;

        hw_state->run_start_time = hw_state->net_time;
        exp_end_time = end_time;

        auto make_steps = [&](int steps){
            int step_size = 0;
            do
            {
                step_size = (steps > 255) ? 255 : steps;
                steps -= step_size;

                if(m_debug)
                    printf(" > STEP %d\n",step_size);

                make_step(send_buf, step_size);
                cur_time += step_size;
            } while(steps > 0);
        };

        // clear fire tracking information
        for(auto &m : hw_state->output_logs) m.clear();

        // First, sort our input fires
        std::sort(input_fires.begin(), input_fires.end(), std::less<InputFireEvent>());

        // Queue fires
        for(auto &&f : input_fires)
        {
            // check if fire is within current time window
            if(f.time < cur_time || f.time > end_time)
            {
                continue;
            }
            else if(f.time > cur_time)
            {
                make_steps(f.time - cur_time);
            }

            make_input_fire(send_buf, f.id, f.weight);
            if(m_debug)
                printf("[t=%3lu] FIRE %3d:%3d\n", cur_time, f.id, f.weight);
        }


        if(cur_time < end_time)
        {
            make_steps(end_time - cur_time);
        }

        if(send_buf.size() > 0) {
            send_and_read(send_buf, [=](HardwareState *hw){ return hw->net_time >= end_time; });
        }

        return true;
    }

    inline void read_fn(struct ftdi_context *ftdi, HardwareState *hw,
            std::function<bool(HardwareState*)> cond)
    {
        std::vector<int> processed_bytes;
        const int max_zero_transfers = 10;

        do
        {
            const int rsz = 15862;
            uint8_t cbuf[rsz];
            int bytes_read = ftdi_read_data(ftdi, cbuf, rsz);
            std::vector<uint8_t> rec(cbuf, cbuf+bytes_read);
            hw->rec_leftover.insert(hw->rec_leftover.end(), rec.begin(), rec.end());

            const size_t processed = hw->parse_cmds_cond(hw->rec_leftover, cond);
            processed_bytes.push_back(processed);

            if(hw->m_debug)
                printf("[TIME: %lu] Processed %lu bytes ", hw->net_time, processed);
            
            if(processed == hw->rec_leftover.size())
            {
                hw->rec_leftover.clear();
            }
            else
            {
                std::vector<uint8_t> new_leftover(hw->rec_leftover.begin()+processed,
                        hw->rec_leftover.end());
                hw->rec_leftover = std::move(new_leftover);
            }

            if(hw->m_debug)
                printf(" - %zu leftover\n", hw->rec_leftover.size());

            if(processed_bytes.size() > max_zero_transfers)
            {
                int processed_last = 0;
                for(size_t i = 0; i < max_zero_transfers; i++)
                {
                    processed_last += processed_bytes[processed_bytes.size()-1-i];
                }

                if(processed_last == 0)
                {
                    if(hw->m_debug) {
                        printf("Processed Bytes: %lu\nExit due to 0 bytes processed "
                                "recently.\n", processed);
                    }
                    // Check FTDI modem status
                    unsigned short ftdi_status;
                    ftdi_poll_modem_status(ftdi, &ftdi_status);

                    printf("Transfer Error | FTDI status: %x\n", ftdi_status);
                    exit(0);
                }
            }
        }
        while(!cond(hw));
    }

    void UsbCaspian::send_and_read(
            std::vector<uint8_t> &buf, std::function<bool(HardwareState*)> &&cond)
    {
        // Make a reader thread using the conditional function
        std::thread reader(read_fn, ftdi, hw_state.get(), cond);

        std::vector<struct ftdi_transfer_control*> sends;

        const int block = 3961; // TODO: What should this be?
        size_t boff = 0;

        while (boff < buf.size()) {

            int sz = std::min(int(buf.size()-boff), block);

            if (m_debug) {

                printf(" < Async write of %d bytes -- offset: %lu -- total: %zu\n",
                        sz, boff, buf.size());

                for (int i=0; i<sz; ++i) {
                    printf("  x%02X\n", buf[boff+i]);
                }
            }

            sends.push_back(ftdi_write_data_submit(ftdi, &(buf[boff]), sz));

            boff += sz;
        }

        reader.join();

        // close out the transmit requests
        for(size_t i = 0; i < sends.size(); i++) ftdi_transfer_data_done(sends[i]);
    }

    uint64_t UsbCaspian::get_time() const
    {
        return hw_state->net_time;
    }

    double UsbCaspian::get_metric(const std::string& metric)
    {
        printf("+++++++++++++++++++++++ get_metric()\n");

        auto mit = metric_addrs.find(metric);
        if(mit == metric_addrs.end() || mit->second.empty())
        {
            printf("Metric '%s' is not implemented for uCaspian.\n",metric.c_str());
            return 0;
        }

        unsigned int metric_bytes = mit->second.size();
        std::vector<uint8_t> buf;
        hw_state->rec_metrics.clear();

        for(auto addr = mit->second.begin(); addr != mit->second.end(); addr++)
        {
            make_get_metric(buf, *addr);
        }

        send_and_read(buf, [=](HardwareState *hw) {
                return hw->rec_metrics.size() >= metric_bytes; 
                });

        int64_t val = 0;

        for(auto metric : hw_state->rec_metrics)
        {
            if(m_debug)
                printf("[METRIC] Addr: %u Value: %u\n", metric.first, metric.second);
            val = val << 8;
            val |= metric.second;
        }

        hw_state->rec_metrics.clear();

        return val;
    }

    void UsbCaspian::reset()
    {
        printf("+++++++++++++++++++++++ reset()\n");

        std::vector<uint8_t> send_buf;
        make_clear_activity(send_buf);
        hw_state->clr_acks = 0;

        send_and_read(send_buf, [](HardwareState *hw){ return hw->clr_acks > 0; });
        
        hw_state->clear_all();
        input_fires.clear();
    }

    bool UsbCaspian::update()
    {
        return true;
    }
    
    Network* UsbCaspian::pull_network(uint32_t /* idx */) const
    {
        // TODO
        return net;
    }

    bool UsbCaspian::track_timing(uint32_t output_id, bool do_tracking)
    {
        printf("+++++++++++++++++++++++ track_timing()\n");
        return hw_state->track_timing(output_id, do_tracking);
    }

    int UsbCaspian::get_output_count(uint32_t output_id, int network_id)
    {
        return hw_state->get_output_count(output_id, network_id);
    }

    int UsbCaspian::get_last_output_time(uint32_t output_id, int network_id)
    {
        return hw_state->get_last_output_time(output_id, network_id);
    }

    std::vector<uint32_t> UsbCaspian::get_output_values(uint32_t output_id, int network_id)
    {
        return hw_state->get_output_values(output_id, network_id);
    }

    bool UsbCaspian::track_aftertime(uint32_t output_id, uint64_t aftertime)
    {
        return hw_state->track_aftertime(output_id, aftertime);
    }

    void UsbCaspian::set_debug(bool debug)
    {
        m_debug = debug;
        if(hw_state) hw_state->m_debug = debug;
    }

    void HardwareState::clear()
    {
        net_time = 0;
        run_start_time = 0;
        clr_acks = 0;
        cfg_acks = 0;

        rec_leftover.clear();
        rec_metrics.clear();

        for(auto &m : output_logs) m.clear();
    }

    void HardwareState::clear_all()
    {
        clear();

        for(auto &a : monitor_aftertime) a = -1;
        for(auto &&p : monitor_precise) p = false;
    }

    void HardwareState::remove_network()
    {
        monitor_aftertime.clear();
        monitor_precise.clear();
        output_logs.clear();
        rec_leftover.clear();
        rec_metrics.clear();
        net = nullptr;
        net_time = 0;
        run_start_time = 0;
        clr_acks = 0;
        cfg_acks = 0;
    }

    void HardwareState::configure(Network *new_net)
    {
        net = new_net;

        monitor_aftertime.resize(net->num_outputs(), -1);
        monitor_precise.resize(net->num_outputs(), false);
        output_logs.emplace_back(net->num_outputs());
        rec_leftover.clear();

        if(m_debug) {
            printf("[configure] outputs: %zu monitor_aftertime: %zu "
                    "monitor_precise: %zu output_logs %zu\n",
                net->num_outputs(),monitor_aftertime.size(),
                monitor_precise.size(), output_logs.size());
        }

        net_time = 0;
        cfg_acks = 0;
        clr_acks = 0;
    }

    bool HardwareState::track_aftertime(uint32_t output_id, uint64_t aftertime)
    {
        if(output_id >= monitor_aftertime.size()) return false;
        monitor_aftertime[output_id] = aftertime;
        return true;
    }
    
    bool HardwareState::track_timing(uint32_t output_id, bool do_tracking)
    {
        if(output_id >= monitor_precise.size()) return false;
        monitor_precise[output_id] = do_tracking;
        return true;
    }

    int HardwareState::get_output_count(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].fire_counts.size()) return -1;
        return output_logs[network_id].fire_counts[output_id];
    }

    int HardwareState::get_last_output_time(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].last_fire_times.size()) return -1;
        return output_logs[network_id].last_fire_times[output_id];
    }

    std::vector<uint32_t> HardwareState::get_output_values(uint32_t output_id, int network_id)
    {
        if(output_id >= output_logs[network_id].recorded_fires.size()) 
            return std::vector<uint32_t>();

        return output_logs[network_id].recorded_fires[output_id];
    }
    
    void UsbCaspian::collect_all_spikes(bool /*collect*/)
    {
    }

    std::vector<std::vector<uint32_t>> UsbCaspian::get_all_spikes()
    {
        return {};
    }

    UsbCaspian::UIntMap UsbCaspian::get_all_spike_cnts()
    {
        return {};
    }

}

#undef CFG_ACK
#undef CLR_ACK
#undef METRIC_RESP
#undef TIME_UPDATE
#undef OUTPUT_FIRE
#endif
