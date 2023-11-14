#include "../include/utils.h"
#include "../include/tracker.h"

using Eigen::MatrixXd;
using Eigen::RowVectorXd;
using cv::Mat;

tracker::tracker () {}

tracker::tracker(int num_of_nodes) {
    // default initialize
    Y_ = MatrixXd::Zero(num_of_nodes, 3);
    guide_nodes_ = Y_.replicate(1, 1);
    sigma2_ = 0.0;
    beta_ = 5.0;
    beta_pre_proc_ = 3.0;
    lambda_ = 1.0;
    lambda_pre_proc_ = 1.0;
    alpha_ = 0.0;
    lle_weight_ = 1.0;
    k_vis_ = 0.0;
    mu_ = 0.05;
    max_iter_ = 50;
    tol_ = 0.00001;
    geodesic_coord_ = {};
    correspondence_priors_ = {};
    visibility_threshold_ = 0.02;
    nodes_per_dlo_ = num_of_nodes;
}

tracker::tracker(int num_of_nodes,
                    int nodes_per_dlo,
                    double visibility_threshold,
                    double beta,
                    double lambda,
                    double alpha,
                    double k_vis,
                    double mu,
                    int max_iter,
                    double tol,
                    double beta_pre_proc,
                    double lambda_pre_proc,
                    double lle_weight) 
{
    Y_ = MatrixXd::Zero(num_of_nodes, 3);
    nodes_per_dlo_ = nodes_per_dlo;
    visibility_threshold_ = visibility_threshold;
    guide_nodes_ = Y_.replicate(1, 1);
    sigma2_ = 0.0;
    beta_ = beta;
    beta_pre_proc_ = beta_pre_proc;
    lambda_ = lambda;
    lambda_pre_proc_ = lambda_pre_proc;
    alpha_ = alpha;
    lle_weight_ = lle_weight;
    k_vis_ = k_vis;
    mu_ = mu;
    max_iter_ = max_iter;
    tol_ = tol;
    geodesic_coord_ = {};
    correspondence_priors_ = {};
}

double tracker::get_sigma2 () {
    return sigma2_;
}

MatrixXd tracker::get_tracking_result () {
    return Y_;
}

MatrixXd tracker::get_guide_nodes () {
    return guide_nodes_;
}

std::vector<MatrixXd> tracker::get_correspondence_pairs () {
    return correspondence_priors_;
}

void tracker::initialize_geodesic_coord (std::vector<double> geodesic_coord) {
    for (int i = 0; i < geodesic_coord.size(); i ++) {
        geodesic_coord_.push_back(geodesic_coord[i]);
    }
}

void tracker::initialize_nodes (MatrixXd Y_init) {
    Y_ = Y_init.replicate(1, 1);
    guide_nodes_ = Y_init.replicate(1, 1);
}

void tracker::set_sigma2 (double sigma2) {
    sigma2_ = sigma2;
}

std::vector<int> tracker::get_nearest_indices (int k, int M, int idx) {
    std::vector<int> indices_arr;
    if (idx - k < 0) {
        for (int i = 0; i <= idx + k; i ++) {
            if (i != idx) {
                indices_arr.push_back(i);
            }
        }
    }
    else if (idx + k >= M) {
        for (int i = idx - k; i <= M - 1; i ++) {
            if (i != idx) {
                indices_arr.push_back(i);
            }
        }
    }
    else {
        for (int i = idx - k; i <= idx + k; i ++) {
            if (i != idx) {
                indices_arr.push_back(i);
            }
        }
    }

    return indices_arr;
}

MatrixXd tracker::calc_LLE_weights (int k, MatrixXd X) {
    MatrixXd W = MatrixXd::Zero(X.rows(), X.rows());
    for (int i = 0; i < X.rows(); i ++) {

        int dlo_index = i / nodes_per_dlo_;
        int offset = dlo_index * nodes_per_dlo_;

        std::vector<int> indices = get_nearest_indices(static_cast<int>(k/2), nodes_per_dlo_, i-offset);
        for (int idx = 0; idx < indices.size(); idx ++) {
            indices[idx] += offset;
        }

        MatrixXd xi = X.row(i);
        MatrixXd Xi = MatrixXd(indices.size(), X.cols());

        // fill in Xi: Xi = X[indices, :]
        for (int r = 0; r < indices.size(); r ++) {
            Xi.row(r) = X.row(indices[r]);
        }

        // component = np.full((len(Xi), len(xi)), xi).T - Xi.T
        MatrixXd component = xi.replicate(Xi.rows(), 1).transpose() - Xi.transpose();
        MatrixXd Gi = component.transpose() * component;
        MatrixXd Gi_inv;

        if (Gi.determinant() != 0) {
            Gi_inv = Gi.inverse();
        }
        else {
            // std::cout << "Gi singular at entry " << i << std::endl;
            double epsilon = 0.00001;
            Gi.diagonal().array() += epsilon;
            Gi_inv = Gi.inverse();
        }

        // wi = Gi_inv * 1 / (1^T * Gi_inv * 1)
        MatrixXd ones_row_vec = MatrixXd::Constant(1, Xi.rows(), 1.0);
        MatrixXd ones_col_vec = MatrixXd::Constant(Xi.rows(), 1, 1.0);

        MatrixXd wi = (Gi_inv * ones_col_vec) / (ones_row_vec * Gi_inv * ones_col_vec).value();
        MatrixXd wi_T = wi.transpose();

        for (int c = 0; c < indices.size(); c ++) {
            W(i, indices[c]) = wi_T(c);
        }
    }

    return W;
}

bool tracker::cpd_lle (MatrixXd X_orig,
                        MatrixXd& Y,
                        double& sigma2,
                        double beta,
                        double lambda,
                        double lle_weight,
                        double mu,
                        int max_iter,
                        double tol,
                        bool include_lle,
                        std::vector<MatrixXd> correspondence_priors,
                        double alpha,
                        std::vector<int> visible_nodes,
                        double k_vis,
                        double visibility_threshold) 
{
    int num_of_dlos = Y.rows() / nodes_per_dlo_;

    // prune X
    MatrixXd X_temp = MatrixXd::Zero(X_orig.rows(), 3);
    int valid_pt_counter = 0;
    for (int i = 0; i < X_orig.rows(); i ++) {
        // find shortest distance between this point and any node
        double shortest_dist = 100000;
        for (int j = 0; j < Y.rows(); j ++) {
            double dist = (Y.row(j) - X_orig.row(i)).norm();
            if (dist < shortest_dist) {
                shortest_dist = dist;
            }
        }
        // require a point to be sufficiently close to the node set to be valid
        if (shortest_dist < 0.1) {
            X_temp.row(valid_pt_counter) = X_orig.row(i);
            valid_pt_counter += 1;
        }
    }
    MatrixXd X = X_temp.topRows(valid_pt_counter);

    bool converged = true;

    int M = Y.rows();
    int N = X.rows();
    int D = 3;

    MatrixXd Y_0 = Y.replicate(1, 1);

    MatrixXd diff_yy = MatrixXd::Zero(M, M);
    MatrixXd diff_yy_sqrt = MatrixXd::Zero(M, M);
    for (int i = 0; i < M; i ++) {
        for (int j = 0; j < M; j ++) {
            diff_yy(i, j) = (Y_0.row(i) - Y_0.row(j)).squaredNorm();
            diff_yy_sqrt(i, j) = (Y_0.row(i) - Y_0.row(j)).norm();
        }
    }

    MatrixXd converted_node_dis = MatrixXd::Zero(M, M); // this is a M*M matrix in place of diff_sqrt
    MatrixXd converted_node_dis_sq = MatrixXd::Zero(M, M);
    std::vector<double> converted_node_coord = {0.0};   // this is not squared

    MatrixXd G = MatrixXd::Zero(M, M);
    double cur_sum = 0;
    for (int i = 0; i < M-1; i ++) {
        cur_sum += pt2pt_dis(Y_0.row(i+1), Y_0.row(i));
        converted_node_coord.push_back(cur_sum);
    }

    for (int i = 0; i < converted_node_coord.size(); i ++) {
        for (int j = 0; j < converted_node_coord.size(); j ++) {
            converted_node_dis_sq(i, j) = pow(converted_node_coord[i] - converted_node_coord[j], 2);
            converted_node_dis(i, j) = abs(converted_node_coord[i] - converted_node_coord[j]);
        }
    }

    // kernel matrix
    G = 1/(2*beta * 2*beta) * (-sqrt(2)*converted_node_dis/beta).array().exp() * (2*converted_node_dis.array() + sqrt(2)*beta);

    // tracking multiple dlos
    if (num_of_dlos > 1) {
        MatrixXd G_new = MatrixXd::Zero(M, M);
        for (int i = 0; i < num_of_dlos; i ++) {
            int start = i * nodes_per_dlo_;
            G_new.block(start, start, nodes_per_dlo_, nodes_per_dlo_) = G.block(start, start, nodes_per_dlo_, nodes_per_dlo_);
        }
        G = G_new.replicate(1, 1);
    }

    // get the LLE matrix
    MatrixXd L = calc_LLE_weights(6, Y_0);
    MatrixXd H = (MatrixXd::Identity(M, M) - L).transpose() * (MatrixXd::Identity(M, M) - L);

    // construct J
    MatrixXd J = MatrixXd::Zero(M, M);
    MatrixXd Y_extended = Y_0.replicate(1, 1);
    if (correspondence_priors.size() != 0) {
        int num_of_correspondence_priors = correspondence_priors.size();

        for (int i = 0; i < num_of_correspondence_priors; i ++) {
            MatrixXd temp = MatrixXd::Zero(1, 3);
            int index = correspondence_priors[i](0, 0);
            temp(0, 0) = correspondence_priors[i](0, 1);
            temp(0, 1) = correspondence_priors[i](0, 2);
            temp(0, 2) = correspondence_priors[i](0, 3);

            J.row(index) = MatrixXd::Identity(M, M).row(index);
            Y_extended.row(index) = temp;

            // // enforce boundaries
            // if (i == 0 || i == num_of_correspondence_priors-1) {
            //     J.row(index) *= 5;
            // }
        }
    }

    // diff_xy should be a (M * N) matrix
    MatrixXd diff_xy = MatrixXd::Zero(M, N);
    for (int i = 0; i < M; i ++) {
        for (int j = 0; j < N; j ++) {
            diff_xy(i, j) = (Y_0.row(i) - X.row(j)).squaredNorm();
        }
    }

    // initialize sigma2
    if (sigma2 == 0) {
        sigma2 = diff_xy.sum() / static_cast<double>(D * M * N);
    }

    for (int it = 0; it < max_iter; it ++) {

        // update diff_xy
        std::map<int, double> shortest_node_pt_dists;
        for (int m = 0; m < M; m ++) {
            // for each node in Y, determine a point in X closest to it
            // for P_vis calculations
            double shortest_dist = 10000;
            for (int n = 0; n < N; n ++) {
                diff_xy(m, n) = (Y.row(m) - X.row(n)).squaredNorm();
                double dist = (Y.row(m) - X.row(n)).norm();
                if (dist < shortest_dist) {
                    shortest_dist = dist;
                }
            }
            // if close enough to X, the node is visible
            if (shortest_dist <= visibility_threshold) {
                shortest_dist = 0;
            }
            // push back the pair
            shortest_node_pt_dists.insert(std::pair<int, double>(m, shortest_dist));
        }

        MatrixXd P = (-0.5 * diff_xy / sigma2).array().exp();
        MatrixXd P_stored = P.replicate(1, 1);
        double c = pow((2 * M_PI * sigma2), static_cast<double>(D)/2) * mu / (1 - mu) * static_cast<double>(M)/N;
        P = P.array().rowwise() / (P.colwise().sum().array() + c);

        // P matrix calculation based on geodesic distance
        std::vector<int> max_p_nodes(P.cols(), 0);
        MatrixXd pts_dis_sq_geodesic = MatrixXd::Zero(M, N);

        // loop through all points
        for (int i = 0; i < N; i ++) {
            
            P.col(i).maxCoeff(&max_p_nodes[i]);
            int max_p_node = max_p_nodes[i];

            int potential_2nd_max_p_node_1 = max_p_node - 1;
            if (potential_2nd_max_p_node_1 == -1) {
                potential_2nd_max_p_node_1 = 2;
            }

            int potential_2nd_max_p_node_2 = max_p_node + 1;
            if (potential_2nd_max_p_node_2 == M) {
                potential_2nd_max_p_node_2 = M - 3;
            }

            int next_max_p_node;
            if (pt2pt_dis(Y.row(potential_2nd_max_p_node_1), X.row(i)) < pt2pt_dis(Y.row(potential_2nd_max_p_node_2), X.row(i))) {
                next_max_p_node = potential_2nd_max_p_node_1;
            } 
            else {
                next_max_p_node = potential_2nd_max_p_node_2;
            }

            // fill the current column of pts_dis_sq_geodesic
            pts_dis_sq_geodesic(max_p_node, i) = pt2pt_dis_sq(Y.row(max_p_node), X.row(i));
            pts_dis_sq_geodesic(next_max_p_node, i) = pt2pt_dis_sq(Y.row(next_max_p_node), X.row(i));

            if (max_p_node < next_max_p_node) {
                for (int j = 0; j < max_p_node; j ++) {
                    pts_dis_sq_geodesic(j, i) = pow(abs(converted_node_coord[j] - converted_node_coord[max_p_node]) + pt2pt_dis(Y.row(max_p_node), X.row(i)), 2);
                }
                for (int j = next_max_p_node; j < M; j ++) {
                    pts_dis_sq_geodesic(j, i) = pow(abs(converted_node_coord[j] - converted_node_coord[next_max_p_node]) + pt2pt_dis(Y.row(next_max_p_node), X.row(i)), 2);
                }
            }
            else {
                for (int j = 0; j < next_max_p_node; j ++) {
                    pts_dis_sq_geodesic(j, i) = pow(abs(converted_node_coord[j] - converted_node_coord[next_max_p_node]) + pt2pt_dis(Y.row(next_max_p_node), X.row(i)), 2);
                }
                for (int j = max_p_node; j < M; j ++) {
                    pts_dis_sq_geodesic(j, i) = pow(abs(converted_node_coord[j] - converted_node_coord[max_p_node]) + pt2pt_dis(Y.row(max_p_node), X.row(i)), 2);
                }
            }
        }

        // update P
        P = (-0.5 * pts_dis_sq_geodesic / sigma2).array().exp();

        
        // modified membership probability (adapted from cdcpd)
        if (visible_nodes.size() != Y.rows() && !visible_nodes.empty() && k_vis != 0) {
            MatrixXd P_vis = MatrixXd::Ones(P.rows(), P.cols());
            double total_P_vis = 0;

            for (int i = 0; i < Y.rows(); i ++) {
                double shortest_node_pt_dist = shortest_node_pt_dists[i];

                double P_vis_i = exp(-k_vis * shortest_node_pt_dist);
                total_P_vis += P_vis_i;

                P_vis.row(i) = P_vis_i * P_vis.row(i);
            }

            // normalize P_vis
            P_vis = P_vis / total_P_vis;

            // modify P
            P = P.cwiseProduct(P_vis);

            // modify c
            c = pow((2 * M_PI * sigma2), static_cast<double>(D)/2) * mu / (1 - mu) / N;
            P = P.array().rowwise() / (P.colwise().sum().array() + c);
        }
        else {
            P = P.array().rowwise() / (P.colwise().sum().array() + c);
        }


        MatrixXd Pt1 = P.colwise().sum();
        MatrixXd P1 = P.rowwise().sum();
        double Np = P1.sum();
        MatrixXd PX = P * X;

        // M step
        MatrixXd A_matrix;
        MatrixXd B_matrix;
        if (include_lle) {
            if (correspondence_priors.size() != 0) {
                A_matrix = P1.asDiagonal()*G + lambda*sigma2 * MatrixXd::Identity(M, M) + sigma2*lle_weight * H*G + alpha*J*G;
                B_matrix = PX - P1.asDiagonal()*Y_0 - sigma2*lle_weight * H*Y_0 + alpha*(Y_extended - Y_0);
            }
            else {
                A_matrix = P1.asDiagonal()*G + lambda*sigma2 * MatrixXd::Identity(M, M) + sigma2*lle_weight * H*G;
                B_matrix = PX - P1.asDiagonal()*Y_0 - sigma2*lle_weight * H*Y_0;
            }
        }
        else {
            if (correspondence_priors.size() != 0) {
                A_matrix = P1.asDiagonal() * G + lambda * sigma2 * MatrixXd::Identity(M, M) + alpha*J*G;
                B_matrix = PX - P1.asDiagonal() * Y_0 + alpha*(Y_extended - Y_0);
            }
            else {
                A_matrix = P1.asDiagonal() * G + lambda * sigma2 * MatrixXd::Identity(M, M);
                B_matrix = PX - P1.asDiagonal() * Y_0;
            }
        }

        MatrixXd W = A_matrix.completeOrthogonalDecomposition().solve(B_matrix);

        MatrixXd T = Y_0 + G * W;
        double trXtdPt1X = (X.transpose() * Pt1.asDiagonal() * X).trace();
        double trPXtT = (PX.transpose() * T).trace();
        double trTtdP1T = (T.transpose() * P1.asDiagonal() * T).trace();

        sigma2 = (trXtdPt1X - 2*trPXtT + trTtdP1T) / (Np * D);

        if (pt2pt_dis(Y, Y_0 + G*W) / Y.rows() < tol) {
            Y = Y_0 + G*W;
            ROS_INFO_STREAM("Iteration until convergence: " + std::to_string(it+1));
            break;
        }
        else {
            Y = Y_0 + G*W;
        }

        if (it == max_iter - 1) {
            ROS_ERROR("optimization did not converge!");
            converged = false;
            break;
        }
    }
    
    return converged;
}

// alignment: 0 --> align with head; 1 --> align with tail
std::vector<MatrixXd> tracker::traverse_geodesic (std::vector<double> geodesic_coord, const MatrixXd guide_nodes, const std::vector<int> visible_nodes, int alignment) {
    std::vector<MatrixXd> node_pairs = {};

    // extreme cases: only one guide node available
    // since this function will only be called when at least one of head or tail is visible, 
    // the only node will be head or tail
    if (guide_nodes.rows() == 1) {
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes[0], guide_nodes(0, 0), guide_nodes(0, 1), guide_nodes(0, 2);
        node_pairs.push_back(node_pair);
        return node_pairs;
    }

    double guide_nodes_total_dist = 0;
    double total_seg_dist = 0;
    
    if (alignment == 0) {
        // push back the first pair
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes[0], guide_nodes(0, 0), guide_nodes(0, 1), guide_nodes(0, 2);
        node_pairs.push_back(node_pair);

        // initialize iterators
        int guide_nodes_it = 0;
        int seg_dist_it = 0;
        int last_seg_dist_it = seg_dist_it;

        // ultimate terminating condition: run out of guide nodes to use. two conditions that can trigger this:
        //   1. next visible node index - current visible node index > 1
        //   2. currenting using the last two guide nodes
        while (visible_nodes[guide_nodes_it+1] - visible_nodes[guide_nodes_it] == 1 && guide_nodes_it+1 <= guide_nodes.rows()-1 && seg_dist_it+1 <= geodesic_coord.size()-1) {
            guide_nodes_total_dist += pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it+1));
            // now keep adding segment dists until the total seg dists exceed the current total guide node dists
            while (guide_nodes_total_dist > total_seg_dist) {
                // break condition
                if (seg_dist_it == geodesic_coord.size()-1) {
                    break;
                }

                total_seg_dist += fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it+1]);
                if (total_seg_dist <= guide_nodes_total_dist) {
                    seg_dist_it += 1;
                }
                else {
                    total_seg_dist -= fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it+1]);
                    break;
                }
            }
            // additional break condition
            if (seg_dist_it == geodesic_coord.size()-1) {
                break;
            }
            // upon exit, seg_dist_it will be at the locaiton where the total seg dist is barely smaller than guide nodes total dist
            // the node desired should be in between guide_nodes[guide_nodes_it] and guide_node[guide_nodes_it + 1]
            // seg_dist_it will also be within guide_nodes_it and guide_nodes_it + 1
            if (guide_nodes_it == 0 && seg_dist_it == 0) {
                continue;
            }
            // if one guide nodes segment is not long enough
            if (last_seg_dist_it == seg_dist_it) {
                guide_nodes_it += 1;
                continue;
            }
            double remaining_dist = total_seg_dist - (guide_nodes_total_dist - pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it+1)));
            MatrixXd temp = (guide_nodes.row(guide_nodes_it + 1) - guide_nodes.row(guide_nodes_it)) * remaining_dist / pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it+1));
            node_pair(0, 0) = seg_dist_it;
            node_pair(0, 1) = temp(0, 0) + guide_nodes(guide_nodes_it, 0);
            node_pair(0, 2) = temp(0, 1) + guide_nodes(guide_nodes_it, 1);
            node_pair(0, 3) = temp(0, 2) + guide_nodes(guide_nodes_it, 2);
            node_pairs.push_back(node_pair);

            // update guide_nodes_it at the very end
            guide_nodes_it += 1;
            last_seg_dist_it = seg_dist_it;
        }
    }
    else {
        // push back the first pair
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes.back(), guide_nodes(guide_nodes.rows()-1, 0), guide_nodes(guide_nodes.rows()-1, 1), guide_nodes(guide_nodes.rows()-1, 2);
        node_pairs.push_back(node_pair);

        // initialize iterators
        int guide_nodes_it = guide_nodes.rows()-1;
        int seg_dist_it = geodesic_coord.size()-1;
        int last_seg_dist_it = seg_dist_it;

        // ultimate terminating condition: run out of guide nodes to use. two conditions that can trigger this:
        //   1. next visible node index - current visible node index > 1
        //   2. currenting using the last two guide nodes
        while (visible_nodes[guide_nodes_it] - visible_nodes[guide_nodes_it-1] == 1 && guide_nodes_it-1 >= 0 && seg_dist_it-1 >= 0) {
            guide_nodes_total_dist += pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it-1));
            // now keep adding segment dists until the total seg dists exceed the current total guide node dists
            while (guide_nodes_total_dist > total_seg_dist) {
                // break condition
                if (seg_dist_it == 0) {
                    break;
                }

                total_seg_dist += fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it-1]);
                if (total_seg_dist <= guide_nodes_total_dist) {
                    seg_dist_it -= 1;
                }
                else {
                    total_seg_dist -= fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it-1]);
                    break;
                }
            }
            // additional break condition
            if (seg_dist_it == 0) {
                break;
            }
            // upon exit, seg_dist_it will be at the locaiton where the total seg dist is barely smaller than guide nodes total dist
            // the node desired should be in between guide_nodes[guide_nodes_it] and guide_node[guide_nodes_it + 1]
            // seg_dist_it will also be within guide_nodes_it and guide_nodes_it + 1
            if (guide_nodes_it == 0 && seg_dist_it == 0) {
                continue;
            }
            // if one guide nodes segment is not long enough
            if (last_seg_dist_it == seg_dist_it) {
                guide_nodes_it -= 1;
                continue;
            }
            double remaining_dist = total_seg_dist - (guide_nodes_total_dist - pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it-1)));
            MatrixXd temp = (guide_nodes.row(guide_nodes_it - 1) - guide_nodes.row(guide_nodes_it)) * remaining_dist / pt2pt_dis(guide_nodes.row(guide_nodes_it), guide_nodes.row(guide_nodes_it-1));
            node_pair(0, 0) = seg_dist_it;
            node_pair(0, 1) = temp(0, 0) + guide_nodes(guide_nodes_it, 0);
            node_pair(0, 2) = temp(0, 1) + guide_nodes(guide_nodes_it, 1);
            node_pair(0, 3) = temp(0, 2) + guide_nodes(guide_nodes_it, 2);
            node_pairs.insert(node_pairs.begin(), node_pair);

            // update guide_nodes_it at the very end
            guide_nodes_it -= 1;
            last_seg_dist_it = seg_dist_it;
        }
    }

    return node_pairs;
}

std::vector<MatrixXd> tracker::traverse_euclidean (std::vector<double> geodesic_coord, const MatrixXd guide_nodes, const std::vector<int> visible_nodes, int alignment, int alignment_node_idx) {
    std::vector<MatrixXd> node_pairs = {};

    // extreme cases: only one guide node available
    // since this function will only be called when at least one of head or tail is visible, 
    // the only node will be head or tail
    if (guide_nodes.rows() == 1) {
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes[0], guide_nodes(0, 0), guide_nodes(0, 1), guide_nodes(0, 2);
        node_pairs.push_back(node_pair);
        return node_pairs;
    }

    if (alignment == 0) {
        // push back the first pair
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes[0], guide_nodes(0, 0), guide_nodes(0, 1), guide_nodes(0, 2);
        node_pairs.push_back(node_pair);

        std::vector<int> consecutive_visible_nodes = {};
        for (int i = 0; i < visible_nodes.size(); i ++) {
            if (i == visible_nodes[i]) {
                consecutive_visible_nodes.push_back(i);
            }
            else {
                break;
            }
        }

        int last_found_index = 0;
        int seg_dist_it = 0;
        MatrixXd cur_center = guide_nodes.row(0);

        // basically pure pursuit
        while (last_found_index+1 <= consecutive_visible_nodes.size()-1 && seg_dist_it+1 <= geodesic_coord.size()-1) {
            double look_ahead_dist = fabs(geodesic_coord[seg_dist_it+1] - geodesic_coord[seg_dist_it]);
            bool found_intersection = false;
            std::vector<double> intersection = {};

            for (int i = last_found_index; i+1 <= consecutive_visible_nodes.size()-1; i ++) {
                std::vector<MatrixXd> intersections = line_sphere_intersection(guide_nodes.row(i), guide_nodes.row(i+1), cur_center, look_ahead_dist);

                // if no intersection found
                if (intersections.size() == 0) {
                    continue;
                }
                else if (intersections.size() == 1 && pt2pt_dis(intersections[0], guide_nodes.row(i+1)) > pt2pt_dis(cur_center, guide_nodes.row(i+1))) {
                    continue;
                }
                else {
                    found_intersection = true;
                    last_found_index = i;

                    if (intersections.size() == 2) {
                        if (pt2pt_dis(intersections[0], guide_nodes.row(i+1)) <= pt2pt_dis(intersections[1], guide_nodes.row(i+1))) {
                            // the first solution is closer
                            intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                            cur_center = intersections[0];
                        }
                        else {
                            // the second one is closer
                            intersection = {intersections[1](0, 0), intersections[1](0, 1), intersections[1](0, 2)};
                            cur_center = intersections[1];
                        }
                    }
                    else {
                        intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                        cur_center = intersections[0];
                    }
                    break;
                }
            }

            if (!found_intersection) {
                break;
            }
            else {
                MatrixXd temp = MatrixXd::Zero(1, 4);
                temp(0, 0) = seg_dist_it + 1;
                temp(0, 1) = intersection[0];
                temp(0, 2) = intersection[1];
                temp(0, 3) = intersection[2];
                node_pairs.push_back(temp);

                seg_dist_it += 1;
            }
        }
    }
    else if (alignment == 1){
        // push back the first pair
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes.back(), guide_nodes(guide_nodes.rows()-1, 0), guide_nodes(guide_nodes.rows()-1, 1), guide_nodes(guide_nodes.rows()-1, 2);
        node_pairs.push_back(node_pair);

        std::vector<int> consecutive_visible_nodes = {};
        for (int i = 1; i <= visible_nodes.size(); i ++) {
            if (visible_nodes[visible_nodes.size()-i] == geodesic_coord.size()-i) {
                consecutive_visible_nodes.push_back(geodesic_coord.size()-i);
            }
            else {
                break;
            }
        }

        int last_found_index = guide_nodes.rows()-1;
        int seg_dist_it = geodesic_coord.size()-1;
        MatrixXd cur_center = guide_nodes.row(guide_nodes.rows()-1);

        // basically pure pursuit
        while (last_found_index-1 >= (guide_nodes.rows() - consecutive_visible_nodes.size()) && seg_dist_it-1 >= 0) {

            double look_ahead_dist = fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it-1]);

            bool found_intersection = false;
            std::vector<double> intersection = {};

            for (int i = last_found_index; i >= (guide_nodes.rows() - consecutive_visible_nodes.size() + 1); i --) {
                std::vector<MatrixXd> intersections = line_sphere_intersection(guide_nodes.row(i), guide_nodes.row(i-1), cur_center, look_ahead_dist);

                // if no intersection found
                if (intersections.size() == 0) {
                    continue;
                }
                else if (intersections.size() == 1 && pt2pt_dis(intersections[0], guide_nodes.row(i-1)) > pt2pt_dis(cur_center, guide_nodes.row(i-1))) {
                    continue;
                }
                else {
                    found_intersection = true;
                    last_found_index = i;

                    if (intersections.size() == 2) {
                        if (pt2pt_dis(intersections[0], guide_nodes.row(i-1)) <= pt2pt_dis(intersections[1], guide_nodes.row(i-1))) {
                            // the first solution is closer
                            intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                            cur_center = intersections[0];
                        }
                        else {
                            // the second one is closer
                            intersection = {intersections[1](0, 0), intersections[1](0, 1), intersections[1](0, 2)};
                            cur_center = intersections[1];
                        }
                    }
                    else {
                        intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                        cur_center = intersections[0];
                    }
                    break;
                }
            }

            if (!found_intersection) {
                break;
            }
            else {
                MatrixXd temp = MatrixXd::Zero(1, 4);
                temp(0, 0) = seg_dist_it - 1;
                temp(0, 1) = intersection[0];
                temp(0, 2) = intersection[1];
                temp(0, 3) = intersection[2];
                node_pairs.push_back(temp);

                seg_dist_it -= 1;
            }
        }
    }
    else {
        // push back the first pair
        MatrixXd node_pair(1, 4);
        node_pair << visible_nodes[alignment_node_idx], guide_nodes(alignment_node_idx, 0), guide_nodes(alignment_node_idx, 1), guide_nodes(alignment_node_idx, 2);
        node_pairs.push_back(node_pair);

        std::vector<int> consecutive_visible_nodes_2 = {visible_nodes[alignment_node_idx]};
        for (int i = alignment_node_idx+1; i < visible_nodes.size(); i ++) {
            if (visible_nodes[i] - visible_nodes[i-1] == 1) {
                consecutive_visible_nodes_2.push_back(visible_nodes[i]);
            }
            else {
                break;
            }
        }

        // traverse from the alignment node to the tail node
        int last_found_index = alignment_node_idx;
        int seg_dist_it = visible_nodes[alignment_node_idx];
        MatrixXd cur_center = guide_nodes.row(alignment_node_idx);

        // basically pure pursuit
        while (last_found_index+1 <= alignment_node_idx+consecutive_visible_nodes_2.size()-1 && seg_dist_it+1 <= geodesic_coord.size()-1) {
            double look_ahead_dist = fabs(geodesic_coord[seg_dist_it+1] - geodesic_coord[seg_dist_it]);
            bool found_intersection = false;
            std::vector<double> intersection = {};

            for (int i = last_found_index; i+1 <= alignment_node_idx+consecutive_visible_nodes_2.size()-1; i ++) {
                std::vector<MatrixXd> intersections = line_sphere_intersection(guide_nodes.row(i), guide_nodes.row(i+1), cur_center, look_ahead_dist);

                // if no intersection found
                if (intersections.size() == 0) {
                    continue;
                }
                else if (intersections.size() == 1 && pt2pt_dis(intersections[0], guide_nodes.row(i+1)) > pt2pt_dis(cur_center, guide_nodes.row(i+1))) {
                    continue;
                }
                else {
                    found_intersection = true;
                    last_found_index = i;

                    if (intersections.size() == 2) {
                        if (pt2pt_dis(intersections[0], guide_nodes.row(i+1)) <= pt2pt_dis(intersections[1], guide_nodes.row(i+1))) {
                            // the first solution is closer
                            intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                            cur_center = intersections[0];
                        }
                        else {
                            // the second one is closer
                            intersection = {intersections[1](0, 0), intersections[1](0, 1), intersections[1](0, 2)};
                            cur_center = intersections[1];
                        }
                    }
                    else {
                        intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                        cur_center = intersections[0];
                    }
                    break;
                }
            }

            if (!found_intersection) {
                break;
            }
            else {
                MatrixXd temp = MatrixXd::Zero(1, 4);
                temp(0, 0) = seg_dist_it + 1;
                temp(0, 1) = intersection[0];
                temp(0, 2) = intersection[1];
                temp(0, 3) = intersection[2];
                node_pairs.push_back(temp);

                seg_dist_it += 1;
            }
        }


        // traverse from alignment node to head node
        std::vector<int> consecutive_visible_nodes_1 = {visible_nodes[alignment_node_idx]};
        for (int i = alignment_node_idx-1; i >= 0; i ++) {
            if (visible_nodes[i+1] - visible_nodes[i] == 1) {
                consecutive_visible_nodes_1.push_back(visible_nodes[i]);
            }
            else {
                break;
            }
        }

        last_found_index = alignment_node_idx;
        seg_dist_it = visible_nodes[alignment_node_idx];
        cur_center = guide_nodes.row(alignment_node_idx);

        // basically pure pursuit
        while (last_found_index-1 >= alignment_node_idx-consecutive_visible_nodes_1.size() && seg_dist_it-1 >= 0) {
            double look_ahead_dist = fabs(geodesic_coord[seg_dist_it] - geodesic_coord[seg_dist_it-1]);
            bool found_intersection = false;
            std::vector<double> intersection = {};

            for (int i = last_found_index; i-1 >= 0; i --) {
                std::vector<MatrixXd> intersections = line_sphere_intersection(guide_nodes.row(i), guide_nodes.row(i-1), cur_center, look_ahead_dist);

                // if no intersection found
                if (intersections.size() == 0) {
                    continue;
                }
                else if (intersections.size() == 1 && pt2pt_dis(intersections[0], guide_nodes.row(i-1)) > pt2pt_dis(cur_center, guide_nodes.row(i-1))) {
                    continue;
                }
                else {
                    found_intersection = true;
                    last_found_index = i;

                    if (intersections.size() == 2) {
                        if (pt2pt_dis(intersections[0], guide_nodes.row(i-1)) <= pt2pt_dis(intersections[1], guide_nodes.row(i-1))) {
                            // the first solution is closer
                            intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                            cur_center = intersections[0];
                        }
                        else {
                            // the second one is closer
                            intersection = {intersections[1](0, 0), intersections[1](0, 1), intersections[1](0, 2)};
                            cur_center = intersections[1];
                        }
                    }
                    else {
                        intersection = {intersections[0](0, 0), intersections[0](0, 1), intersections[0](0, 2)};
                        cur_center = intersections[0];
                    }
                    break;
                }
            }

            if (!found_intersection) {
                break;
            }
            else {
                MatrixXd temp = MatrixXd::Zero(1, 4);
                temp(0, 0) = seg_dist_it - 1;
                temp(0, 1) = intersection[0];
                temp(0, 2) = intersection[1];
                temp(0, 3) = intersection[2];
                node_pairs.push_back(temp);

                seg_dist_it -= 1;
            }
        }
    }

    return node_pairs;
}

void tracker::tracking_step (MatrixXd X_orig, 
                              std::vector<int> visible_nodes, 
                              std::vector<int> visible_nodes_extended, 
                              MatrixXd proj_matrix, 
                              int img_rows, 
                              int img_cols) {
    
    // variable initialization
    correspondence_priors_ = {};
    int state = 0;

    // copy visible nodes vec to guide nodes
    // not using topRows() because it caused weird bugs
    guide_nodes_ = MatrixXd::Zero(visible_nodes_extended.size(), 3);
    if (visible_nodes_extended.size() != Y_.rows()) {
        for (int i = 0; i < visible_nodes_extended.size(); i ++) {
            guide_nodes_.row(i) = Y_.row(visible_nodes_extended[i]);
        }
    }
    else {
        guide_nodes_ = Y_.replicate(1, 1);
    }

    // determine DLO state: heading visible, tail visible, both visible, or both occluded
    // priors_vec should be the final output; priors_vec[i] = {index, x, y, z}
    double sigma2_pre_proc = sigma2_;
    // pre-processing registration
    cpd_lle(X_orig, guide_nodes_, sigma2_pre_proc, beta_pre_proc_, lambda_pre_proc_, lle_weight_, mu_, max_iter_, tol_, true);

    int num_of_dlos = Y_.rows() / nodes_per_dlo_;

    std::cout << "== visible_nodes_extended ==" << std::endl;
    print_1d_vector(visible_nodes_extended);

    int visible_nodes_extended_index = 0;
    for (int dlo_idx = 0; dlo_idx < num_of_dlos; dlo_idx ++) {
        std::cout << "===== dlo #" + std::to_string(dlo_idx+1) + " =====" << std::endl;

        // get y sub
        MatrixXd Y_sub = Y_.block(dlo_idx*nodes_per_dlo_, 0, nodes_per_dlo_, 3);

        // get guide nodes sub
        MatrixXd guide_nodes_sub = guide_nodes_.block(dlo_idx*nodes_per_dlo_, 0, nodes_per_dlo_, 3);

        // get visible nodes sub
        std::vector<int> visible_nodes_extended_sub = {};
        int id = visible_nodes_extended[visible_nodes_extended_index];
        while (id < (dlo_idx+1)*nodes_per_dlo_) {
            if (visible_nodes_extended_index == Y_.rows()) {
                break;
            }

            // indices in visible nodes need to be within the range of [0, nodes_per_dlo_]; otherwise traversal won't work
            visible_nodes_extended_sub.push_back(id - dlo_idx*nodes_per_dlo_);
            visible_nodes_extended_index += 1;

            // std::cout << "visible_nodes_extended_index = " << visible_nodes_extended_index << std::endl;
            id = visible_nodes_extended[visible_nodes_extended_index];
            // std::cout << "id = " << id << std::endl;
        }
        std::cout << "===== visible_nodes_extended_sub =====" << std::endl;
        print_1d_vector(visible_nodes_extended_sub);

        // get geodesic coord sub
        std::vector<double> geodesic_coord_sub = {};
        for (int i = dlo_idx*nodes_per_dlo_; i < (dlo_idx+1)*nodes_per_dlo_; i ++) {
            geodesic_coord_sub.push_back(geodesic_coord_[i]);
        }

        // get corr priors
        if (visible_nodes_extended_sub.size() == Y_sub.rows()) {
            ROS_INFO("All nodes visible or minor occlusion");

            // remap visible node locations
            std::vector<MatrixXd> priors_vec_1 = traverse_euclidean(geodesic_coord_sub, guide_nodes_sub, visible_nodes_extended_sub, 0);
            std::vector<MatrixXd> priors_vec_2 = traverse_euclidean(geodesic_coord_sub, guide_nodes_sub, visible_nodes_extended_sub, 1);

            // priors vec 2 goes from last index -> first index
            std::reverse(priors_vec_2.begin(), priors_vec_2.end());

            std::cout << "===== priors_vec_1 =====" << std::endl;
            print_1d_vector(priors_vec_1);
            std::cout << "===== priors_vec_2 =====" << std::endl;
            print_1d_vector(priors_vec_2);

            // take average
            MatrixXd offset(1, 4);
            offset << dlo_idx*nodes_per_dlo_, 0, 0, 0;

            for (int i = 0; i < Y_sub.rows(); i ++) {
                if (i < priors_vec_2[0](0, 0) && i < priors_vec_1.size()) {
                    correspondence_priors_.push_back(priors_vec_1[i] + offset);
                }
                else if (i > priors_vec_1[priors_vec_1.size()-1](0, 0) && (i-(Y_sub.rows()-priors_vec_2.size())) < priors_vec_2.size()) {
                    correspondence_priors_.push_back(priors_vec_2[i-(Y_sub.rows()-priors_vec_2.size())] + offset);
                }
                else {
                    correspondence_priors_.push_back((priors_vec_1[i] + priors_vec_2[i-(Y_sub.rows()-priors_vec_2.size())]) / 2.0 + offset);
                }
            }
        }
        else {
            std::cout << "not supported yet" << std::endl;
        }
        // else if (visible_nodes_extended[0] == 0 && visible_nodes_extended[visible_nodes_extended.size()-1] == Y_.rows()-1) {
        //     ROS_INFO("Mid-section occluded");

        //     correspondence_priors_ = traverse_euclidean(geodesic_coord_, guide_nodes_, visible_nodes_extended, 0);
        //     std::vector<MatrixXd> priors_vec_2 = traverse_euclidean(geodesic_coord_, guide_nodes_, visible_nodes_extended, 1);
        //     // priors_vec = traverse_geodesic(geodesic_coord, guide_nodes, visible_nodes, 0);
        //     // std::vector<MatrixXd> priors_vec_2 = traverse_geodesic(geodesic_coord, guide_nodes, visible_nodes, 1);

        //     correspondence_priors_.insert(correspondence_priors_.end(), priors_vec_2.begin(), priors_vec_2.end());
        // }
        // else if (visible_nodes_extended[0] == 0) {
        //     ROS_INFO("Tail occluded");

        //     correspondence_priors_ = traverse_euclidean(geodesic_coord_, guide_nodes_, visible_nodes_extended, 0);
        //     // priors_vec = traverse_geodesic(geodesic_coord, guide_nodes, visible_nodes, 0);
        // }
        // else if (visible_nodes_extended[visible_nodes_extended.size()-1] == Y_.rows()-1) {
        //     ROS_INFO("Head occluded");

        //     correspondence_priors_ = traverse_euclidean(geodesic_coord_, guide_nodes_, visible_nodes_extended, 1);
        //     // priors_vec = traverse_geodesic(geodesic_coord, guide_nodes, visible_nodes, 1);
        // }
        // else {
        //     ROS_INFO("Both ends occluded");

        //     // determine which node moved the least
        //     int alignment_node_idx = -1;
        //     double moved_dist = 999999;
        //     for (int i = 0; i < visible_nodes.size(); i ++) {
        //         if (pt2pt_dis(Y_.row(visible_nodes[i]), guide_nodes_.row(i)) < moved_dist) {
        //             moved_dist = pt2pt_dis(Y_.row(visible_nodes[i]), guide_nodes_.row(i));
        //             alignment_node_idx = i;
        //         }
        //     }

        //     // std::cout << "alignment node index: " << alignment_node_idx << std::endl;
        //     correspondence_priors_ = traverse_euclidean(geodesic_coord_, guide_nodes_, visible_nodes_extended, 2, alignment_node_idx);
        // }
    }
    std::cout << "===== correspondence_priors_ =====" << std::endl;
    print_1d_vector(correspondence_priors_);

    // include_lle == false because we have no space to discuss it in the paper
    cpd_lle (X_orig, Y_, sigma2_, beta_, lambda_, lle_weight_, mu_, max_iter_, tol_, false, correspondence_priors_, alpha_, visible_nodes_extended, k_vis_, visibility_threshold_);
}