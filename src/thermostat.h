#include <cstdint>
#include <cmath>

struct OrnsteinUhlenbeckThermostat
        // following the notation in Gillespie, 1996
{
    private: 
        int   n_invocations;
        void update_parameters() {
            mom_scale   = exp(-delta_t/timescale);
            noise_scale = sqrt(temp * (1-mom_scale*mom_scale));
        }

    public:
        uint32_t random_seed;
        float timescale;
        float temp;
        float delta_t;
        float mom_scale;
        float noise_scale;

        OrnsteinUhlenbeckThermostat(uint32_t random_seed_, float timescale_, float temp_, float delta_t_):
            n_invocations(0),
            random_seed(random_seed_), timescale(timescale_), temp(temp_), delta_t(delta_t_)
            {update_parameters();}

        OrnsteinUhlenbeckThermostat& set_timescale(float timescale_) {
            timescale = timescale_; update_parameters(); return *this;}
        OrnsteinUhlenbeckThermostat& set_temp     (float temp_)      {
            temp      = temp_;      update_parameters(); return *this;}
        OrnsteinUhlenbeckThermostat& set_delta_t  (float delta_t_)   {
            delta_t   = delta_t_;   update_parameters(); return *this;}

        void apply(float* mom, int n_atom); 
};
