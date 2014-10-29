#include "md.h"
#include "coord.h"
#include "md_export.h"

#include <vector>

#define HMM_NORMALIZATION_INTERVAL 4


namespace {

struct HMMDeriv {
    float3 phi_x1;
    float3 phi_x2;
    float3 phi_x3;
    float3 phi_x4;

    float3 psi_x2;
    float3 psi_x3;
    float3 psi_x4;
    float3 psi_x5;

    float2 pot_rama[N_STATE];
};

inline void approx_normalization(float* x, int n) 
{
    float total = 0.f;
    for(int i=0; i<n; ++i) total += x[i];
    float inv_total = 1.f/total;
    for(int i=0; i<n; ++i) x[i] *= inv_total;
}

struct RamaMap {

    int const n_layer;
    int const n_bin;
    const RamaMapGerm* const data;

    RamaMap(int n_layer_, int n_bin_, const RamaMapGerm* data_):
        n_layer(n_layer_), n_bin(n_bin_), data(data_) {}

    RamaMapGerm read_map(int layer, float phi, float psi) {
        RamaMapGerm result;

        // map phi and psi into dimension 0 to n_bin
        float phi_coord = (phi+M_PI_F) * (0.5*M_1_PI_F*n_bin) - 0.5f;
        float psi_coord = (psi+M_PI_F) * (0.5*M_1_PI_F*n_bin) - 0.5f;

        if(phi_coord<0.f) phi_coord += n_bin;
        if(psi_coord<0.f) psi_coord += n_bin;

        // order of comparisons is correct in case of NaN
        if(!((phi_coord>=0.f) & (phi_coord<=n_bin) & (psi_coord>=0.f) & (psi_coord<=n_bin))) {
            for(int ns=0; ns<N_STATE; ++ns) {
                result.val [ns] = NAN;
                result.dphi[ns] = NAN;
                result.dpsi[ns] = NAN;
            }
            return result;
        }

        int phi_l_bin = phi_coord;
        int psi_l_bin = psi_coord;

        // handle periodicity
        int phi_r_bin = phi_l_bin==n_bin-1 ? 0 : phi_l_bin+1;
        int psi_r_bin = psi_l_bin==n_bin-1 ? 0 : psi_l_bin+1;

        int i_ll = layer*n_bin*n_bin + phi_l_bin*n_bin + psi_l_bin;
        int i_lr = layer*n_bin*n_bin + phi_l_bin*n_bin + psi_r_bin;
        int i_rl = layer*n_bin*n_bin + phi_r_bin*n_bin + psi_l_bin;
        int i_rr = layer*n_bin*n_bin + phi_r_bin*n_bin + psi_r_bin;

        float phi_r_weight = phi_coord - phi_l_bin;
        float psi_r_weight = psi_coord - psi_l_bin;

        float phi_l_weight = 1.f - phi_r_weight;
        float psi_l_weight = 1.f - psi_r_weight;

        float c_ll = phi_l_weight * psi_l_weight;
        float c_lr = phi_l_weight * psi_r_weight;
        float c_rl = phi_r_weight * psi_l_weight;
        float c_rr = phi_r_weight * psi_r_weight;


        for(int ns=0; ns<N_STATE; ++ns) {
            result.val [ns] = c_ll*data[i_ll].val [ns] + c_lr*data[i_lr].val [ns] + c_rl*data[i_rl].val [ns] + c_rr*data[i_rr].val [ns];
            result.dphi[ns] = c_ll*data[i_ll].dphi[ns] + c_lr*data[i_lr].dphi[ns] + c_rl*data[i_rl].dphi[ns] + c_rr*data[i_rr].dphi[ns];
            result.dpsi[ns] = c_ll*data[i_ll].dpsi[ns] + c_lr*data[i_lr].dpsi[ns] + c_rl*data[i_rl].dpsi[ns] + c_rr*data[i_rr].dpsi[ns];
        }

        return result;
    }
};



template <typename StaticCoordT>
void hmm_stage1(
        const StaticCoordT &x1,
        const StaticCoordT &x2,
        const StaticCoordT &x3,
        const StaticCoordT &x4,
        const StaticCoordT &x5,
        HMMDeriv &d,
        RamaMap &rama,
        float * basin_prob,   // (n_residue, n_state, n_system)
        int residue_number)
{
    float phi = dihedral_germ(x1.f3(),x2.f3(),x3.f3(),x4.f3(), d.phi_x1,d.phi_x2,d.phi_x3,d.phi_x4);
    float psi = dihedral_germ(x2.f3(),x3.f3(),x4.f3(),x5.f3(), d.psi_x2,d.psi_x3,d.psi_x4,d.psi_x5);

    RamaMapGerm rama_germ = rama.read_map(residue_number, phi, psi);
        // FIXME why is this psi before phi?  the results are wrong otherwise

    for(int ns=0; ns<N_STATE; ++ns) {
        // the 1e-6 is a hack to avoid a situation where all basin probs are exactly zero,
        // causing NaN in later calculation.
        basin_prob[residue_number*N_STATE+ns] = rama_germ.val[ns] + 1e-6f;  
        d.pot_rama[ns] = make_float2(rama_germ.dphi[ns], rama_germ.dpsi[ns]);
    }
}



// storage order is forward_prob then backward_prob
#define fb(forward_or_backward, nres, nstate) \
    (forward_backward_prob	\
     [(forward_or_backward)*n_residue*N_STATE + (nres)*N_STATE + (nstate)])

void
hmm_stage2(
        float*   forward_backward_prob,
        int n_residue,
        const float* basin_prob,   // (n_res, N_STATE, n_system)
        const float* trans_matrices,
        int is_backward)
{
    #define STORE_PROB  for(int ns=0; ns<N_STATE; ++ns)  fb(backward, (nr+1), ns) = prob[ns]
    #define NS N_STATE

    if(!is_backward) {  // forward iteration
        float prob[NS];
        for(int ns=0; ns<NS; ++ns) fb(is_backward, 0, ns) = prob[ns] = 1.f;

        for(int nr=0; nr<n_residue-1; ++nr) {
	    // first perform basin-scaling
	    for(int ns=0; ns<NS; ++ns) prob[ns] *= basin_prob[nr*NS+ns];

	    // right multiply by the transition matrix
            float new_prob[NS];
            for(int ns=0; ns<NS; ++ns) new_prob[ns] = 0.f;

            for(int from_state=0; from_state<NS; ++from_state)  {
                for(int ns=0; ns<NS; ++ns) 
                    new_prob[ns] += prob[from_state] * trans_matrices[nr*NS*NS + from_state*NS + ns];
            }
            for(int ns=0; ns<NS; ++ns) prob[ns] = new_prob[ns];

            if(!(nr%HMM_NORMALIZATION_INTERVAL)) approx_normalization(prob, NS);
            for(int ns=0; ns<NS; ++ns) fb(is_backward, nr+1, ns) = prob[ns];
        }
    } else {
        float prob[NS];
        for(int ns=0; ns<NS; ++ns) fb(is_backward, n_residue-1, ns) = prob[ns] = 1.f;

        for(int nr=n_residue-1; nr>0; --nr) {
	    // first perform basin-scaling
	    for(int ns=0; ns<NS; ++ns) prob[ns] *= basin_prob[nr*NS+ns];

	    // left multiply by the transition matrix
            float new_prob[NS];
            for(int ns=0; ns<NS; ++ns) new_prob[ns] = 0.f;

            for(int from_state=0; from_state<NS; ++from_state) {
                for(int ns=0; ns<NS; ++ns) {
                    new_prob[ns] += trans_matrices[(nr-1)*NS*NS + ns*NS + from_state] * prob[from_state];
                }
            }
            for(int ns=0; ns<NS; ++ns) prob[ns] = new_prob[ns];

            if(!(nr%HMM_NORMALIZATION_INTERVAL)) approx_normalization(prob, NS);
            for(int ns=0; ns<NS; ++ns) fb(is_backward, nr-1, ns) = prob[ns];
        }
    }
    #undef STORE_PROB
    #undef NS
}


template <typename MutableCoordT>
void hmm_stage3(
        MutableCoordT &x1d,
        MutableCoordT &x2d,
        MutableCoordT &x3d,
        MutableCoordT &x4d,
        MutableCoordT &x5d,
        float* accum_buffer,
        const float * basin_prob,   // (n_residue, n_state, n_system)
        HMMDeriv &d,
        const float * forward_backward_prob,
        int n_residue,
        int nr)
{
    float prob[N_STATE];
    float inv_sum_prob = 0.f;
    for(int ns=0; ns<N_STATE; ++ns) {
      prob[ns] = fb(0,nr,ns) * basin_prob[nr*N_STATE + ns] * fb(1,nr,ns);
      inv_sum_prob += prob[ns];
    }
    inv_sum_prob = 1.f / inv_sum_prob;

    // find the derivative with respect to phi/psi
    float2 deriv = make_float2(0.f, 0.f);
    for(int ns=0; ns<N_STATE; ++ns) {
        float prefactor = prob[ns]*inv_sum_prob;
        deriv.x += prefactor * d.pot_rama[ns].x;
        deriv.y += prefactor * d.pot_rama[ns].y;
    }

    x1d.set_value(deriv.x*d.phi_x1                   );
    x2d.set_value(deriv.x*d.phi_x2 + deriv.y*d.psi_x2);
    x3d.set_value(deriv.x*d.phi_x3 + deriv.y*d.psi_x3);
    x4d.set_value(deriv.x*d.phi_x4 + deriv.y*d.psi_x4);
    x5d.set_value(                   deriv.y*d.psi_x5);
}
 #undef fb
}

void hmm(
        const float* restrict pos,
        float* restrict pos_deriv,
        const HMMParams* restrict params,
        const float* restrict trans_matrices,
        int n_bin,
        const RamaMapGerm* restrict rama_map_data,
        int n_residue) 
{
    std::vector<HMMDeriv> deriv(n_residue);
    std::vector<float> basin_prob (n_residue*N_STATE);
    std::vector<float> forward_backward_prob(n_residue*2*N_STATE);
    RamaMap rama_map(n_residue, n_bin, rama_map_data);

    for(int nt=0; nt<n_residue; ++nt) {
        StaticCoord<3> x1(pos, params[nt].atom[0].index);
        StaticCoord<3> x2(pos, params[nt].atom[1].index);
        StaticCoord<3> x3(pos, params[nt].atom[2].index);
        StaticCoord<3> x4(pos, params[nt].atom[3].index);
        StaticCoord<3> x5(pos, params[nt].atom[4].index);

        hmm_stage1(x1,x2,x3,x4,x5, deriv[nt], rama_map, basin_prob.data(), nt);
    }

    for(int is_backward=0; is_backward<2; ++is_backward) {
        hmm_stage2(forward_backward_prob.data(),
                n_residue,
                basin_prob.data(),
                trans_matrices,
                is_backward);
    }

    for(int nt=0; nt<n_residue; ++nt) {
        MutableCoord<3> x1d(pos_deriv, params[nt].atom[0].slot, MutableCoord<3>::Zero);
        MutableCoord<3> x2d(pos_deriv, params[nt].atom[1].slot, MutableCoord<3>::Zero);
        MutableCoord<3> x3d(pos_deriv, params[nt].atom[2].slot, MutableCoord<3>::Zero);
        MutableCoord<3> x4d(pos_deriv, params[nt].atom[3].slot, MutableCoord<3>::Zero);
        MutableCoord<3> x5d(pos_deriv, params[nt].atom[4].slot, MutableCoord<3>::Zero);

        hmm_stage3(
                x1d,x2d,x3d,x4d,x5d,
                pos_deriv,
                basin_prob.data(),
                deriv[nt],
                forward_backward_prob.data(),
                n_residue,
                nt);

        x1d.flush();
        x2d.flush();
        x3d.flush();
        x4d.flush();
        x5d.flush();
    }
}
