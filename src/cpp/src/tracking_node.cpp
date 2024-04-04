#include "../include/tracker.h"
#include "../include/utils.h"

using cv::Mat;

ros::Publisher pc_pub;
ros::Publisher results_pub;
ros::Publisher guide_nodes_pub;
ros::Publisher corr_priors_pub;
ros::Publisher self_occluded_pc_pub;
ros::Publisher result_pc_pub;
ros::Subscriber init_nodes_sub;
ros::Subscriber camera_info_sub;

using Eigen::MatrixXd;
using Eigen::RowVectorXd;
using Eigen::MatrixXi;
using Eigen::Matrix2Xi;

MatrixXd Y;
double sigma2;
bool initialized = false;
bool received_init_nodes = false;
bool received_proj_matrix = false;
MatrixXd init_nodes;
std::vector<double> converted_node_coord = {0.0};
Mat occlusion_mask;
bool updated_opencv_mask = false;
MatrixXd proj_matrix(3, 4);

bool multi_color_dlo;
double beta;
double lambda;
double alpha;
double lle_weight;
double mu;
int max_iter;
double tol;
bool include_lle;
bool use_geodesic;
bool use_prev_sigma2;
double downsample_leaf_size;
int nodes_per_dlo;
double dlo_diameter;
double check_distance;
bool clamp;

double visibility_threshold;
int dlo_pixel_width;
double d_vis;
double k_vis;
double beta_pre_proc;
double lambda_pre_proc;

std::string camera_info_topic;
std::string rgb_topic;
std::string depth_topic;
std::string hsv_threshold_upper_limit;
std::string hsv_threshold_lower_limit;
std::string result_frame_id;
std::vector<int> upper;
std::vector<int> lower;

tracker multi_dlo_tracker;

void update_opencv_mask (const sensor_msgs::ImageConstPtr& opencv_mask_msg) {
    occlusion_mask = cv_bridge::toCvShare(opencv_mask_msg, "bgr8")->image;
    if (!occlusion_mask.empty()) {
        updated_opencv_mask = true;
    }
}

void update_init_nodes (const sensor_msgs::PointCloud2ConstPtr& pc_msg) {
    pcl::PCLPointCloud2* cloud = new pcl::PCLPointCloud2;
    pcl_conversions::toPCL(*pc_msg, *cloud);
    pcl::PointCloud<pcl::PointXYZRGB> cloud_xyz;
    pcl::fromPCLPointCloud2(*cloud, cloud_xyz);

    init_nodes = cloud_xyz.getMatrixXfMap().topRows(3).transpose().cast<double>();
    received_init_nodes = true;
    init_nodes_sub.shutdown();
}

void update_camera_info (const sensor_msgs::CameraInfoConstPtr& cam_msg) {
    auto P = cam_msg->P;
    for (int i = 0; i < P.size(); i ++) {
        proj_matrix(i/4, i%4) = P[i];
    }
    received_proj_matrix = true;
    camera_info_sub.shutdown();
}

double pre_proc_total = 0;
double algo_total = 0;
double pub_data_total = 0;
int frames = 0;

Mat color_thresholding (Mat cur_image_hsv) {
    std::vector<int> lower_blue = {90, 90, 60};
    std::vector<int> upper_blue = {130, 255, 255};

    std::vector<int> lower_red_1 = {130, 60, 50};
    std::vector<int> upper_red_1 = {255, 255, 255};

    std::vector<int> lower_red_2 = {0, 60, 50};
    std::vector<int> upper_red_2 = {10, 255, 255};

    std::vector<int> lower_yellow = {15, 100, 80};
    std::vector<int> upper_yellow = {40, 255, 255};

    Mat mask_blue, mask_red_1, mask_red_2, mask_red, mask_yellow, mask;
    // filter blue
    cv::inRange(cur_image_hsv, cv::Scalar(lower_blue[0], lower_blue[1], lower_blue[2]), cv::Scalar(upper_blue[0], upper_blue[1], upper_blue[2]), mask_blue);

    // filter red
    cv::inRange(cur_image_hsv, cv::Scalar(lower_red_1[0], lower_red_1[1], lower_red_1[2]), cv::Scalar(upper_red_1[0], upper_red_1[1], upper_red_1[2]), mask_red_1);
    cv::inRange(cur_image_hsv, cv::Scalar(lower_red_2[0], lower_red_2[1], lower_red_2[2]), cv::Scalar(upper_red_2[0], upper_red_2[1], upper_red_2[2]), mask_red_2);

    // filter yellow
    cv::inRange(cur_image_hsv, cv::Scalar(lower_yellow[0], lower_yellow[1], lower_yellow[2]), cv::Scalar(upper_yellow[0], upper_yellow[1], upper_yellow[2]), mask_yellow);

    // combine red mask
    cv::bitwise_or(mask_red_1, mask_red_2, mask_red);
    // combine overall mask
    cv::bitwise_or(mask_red, mask_blue, mask);
    cv::bitwise_or(mask_yellow, mask, mask);

    return mask;
}

sensor_msgs::ImagePtr Callback(const sensor_msgs::ImageConstPtr& image_msg, const sensor_msgs::ImageConstPtr& depth_msg) {

    Mat cur_image_orig = cv_bridge::toCvShare(image_msg, "bgr8")->image;
    Mat cur_depth = cv_bridge::toCvShare(depth_msg, depth_msg->encoding)->image;

    // will get overwritten later if intialized
    sensor_msgs::ImagePtr tracking_img_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", cur_image_orig).toImageMsg();
    
    if (!initialized) {
        if (received_init_nodes && received_proj_matrix) {
            // tracker(int num_of_nodes,
            //     int nodes_per_dlo,
            //     double visibility_threshold,
            //     double beta,
            //     double lambda,
            //     double alpha,
            //     double k_vis,
            //     double mu,
            //     int max_iter,
            //     double tol,
            //     double beta_pre_proc,
            //     double lambda_pre_proc,
            //     double lle_weight);
            multi_dlo_tracker = tracker(init_nodes.rows(), nodes_per_dlo, visibility_threshold, beta, lambda, alpha, k_vis, mu, max_iter, tol, beta_pre_proc, lambda_pre_proc, lle_weight);

            sigma2 = 0.00001;

            // record geodesic coord
            double cur_sum = 0;
            for (int i = 0; i < init_nodes.rows()-1; i ++) {
                cur_sum += (init_nodes.row(i+1) - init_nodes.row(i)).norm();
                if ((i+1) % nodes_per_dlo == 0 && i != 1) {
                    cur_sum = 0;
                }
                converted_node_coord.push_back(cur_sum);
            }

            multi_dlo_tracker.initialize_nodes(init_nodes);
            multi_dlo_tracker.initialize_geodesic_coord(converted_node_coord);
            Y = init_nodes.replicate(1, 1);

            initialized = true;
        }
    }
    else {
        // log time
        std::chrono::high_resolution_clock::time_point cur_time_cb = std::chrono::high_resolution_clock::now();
        double time_diff;
        std::chrono::high_resolution_clock::time_point cur_time;

        Mat mask, mask_rgb, mask_without_occlusion_block;
        Mat cur_image_hsv;

        // convert color
        cv::cvtColor(cur_image_orig, cur_image_hsv, cv::COLOR_BGR2HSV);

        if (!multi_color_dlo) {
            // color_thresholding
            cv::inRange(cur_image_hsv, cv::Scalar(lower[0], lower[1], lower[2]), cv::Scalar(upper[0], upper[1], upper[2]), mask_without_occlusion_block);
        }
        else {
            mask_without_occlusion_block = color_thresholding(cur_image_hsv);
        }

        // update cur image for visualization
        Mat cur_image;
        Mat occlusion_mask_gray;
        if (updated_opencv_mask) {
            cv::cvtColor(occlusion_mask, occlusion_mask_gray, cv::COLOR_BGR2GRAY);
            cv::bitwise_and(mask_without_occlusion_block, occlusion_mask_gray, mask);
            cv::bitwise_and(cur_image_orig, occlusion_mask, cur_image);
        }
        else {
            mask_without_occlusion_block.copyTo(mask);
            cur_image_orig.copyTo(cur_image);
        }

        cv::cvtColor(mask, mask_rgb, cv::COLOR_GRAY2BGR);

        bool simulated_occlusion = false;
        int occlusion_corner_i = -1;
        int occlusion_corner_j = -1;
        int occlusion_corner_i_2 = -1;
        int occlusion_corner_j_2 = -1;

        // filter point cloud
        pcl::PointCloud<pcl::PointXYZRGB> cur_pc;
        pcl::PointCloud<pcl::PointXYZRGB> cur_pc_downsampled;

        // filter point cloud from mask
        for (int i = 0; i < mask.rows; i ++) {
            for (int j = 0; j < mask.cols; j ++) {
                // for text label (visualization)
                if (updated_opencv_mask && !simulated_occlusion && occlusion_mask_gray.at<uchar>(i, j) == 0) {
                    occlusion_corner_i = i;
                    occlusion_corner_j = j;
                    simulated_occlusion = true;
                }

                // update the other corner of occlusion mask (visualization)
                if (updated_opencv_mask && occlusion_mask_gray.at<uchar>(i, j) == 0) {
                    occlusion_corner_i_2 = i;
                    occlusion_corner_j_2 = j;
                }

                if (mask.at<uchar>(i, j) != 0) {
                    // point cloud from image pixel coordinates and depth value
                    pcl::PointXYZRGB point;
                    double pixel_x = static_cast<double>(j);
                    double pixel_y = static_cast<double>(i);
                    double cx = proj_matrix(0, 2);
                    double cy = proj_matrix(1, 2);
                    double fx = proj_matrix(0, 0);
                    double fy = proj_matrix(1, 1);
                    double pc_z = cur_depth.at<uint16_t>(i, j) / 1000.0;

                    point.x = (pixel_x - cx) * pc_z / fx;
                    point.y = (pixel_y - cy) * pc_z / fy;
                    point.z = pc_z;

                    // currently something so color doesn't show up in rviz
                    // point.r = cur_image_orig.at<cv::Vec3b>(i, j)[0];
                    // point.g = cur_image_orig.at<cv::Vec3b>(i, j)[1];
                    // point.b = cur_image_orig.at<cv::Vec3b>(i, j)[2];
                    point.r = 255;
                    point.g = 255;
                    point.b = 255;

                    cur_pc.push_back(point);
                }
            }
        }

        // Perform downsampling
        pcl::PointCloud<pcl::PointXYZRGB>::ConstPtr cloudPtr(cur_pc.makeShared());
        pcl::VoxelGrid<pcl::PointXYZRGB> sor;
        sor.setInputCloud (cloudPtr);
        sor.setLeafSize (downsample_leaf_size, downsample_leaf_size, downsample_leaf_size);
        sor.filter(cur_pc_downsampled);

        MatrixXd X = cur_pc_downsampled.getMatrixXfMap().topRows(3).transpose().cast<double>();
        ROS_INFO_STREAM("Number of points in downsampled point cloud: " + std::to_string(X.rows()));

        MatrixXd guide_nodes;
        std::vector<MatrixXd> priors;

        // log time
        time_diff = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - cur_time_cb).count() / 1000.0;
        ROS_INFO_STREAM("Before tracking step: " + std::to_string(time_diff) + " ms");
        pre_proc_total += time_diff;
        cur_time = std::chrono::high_resolution_clock::now();

        int num_of_dlos = Y.rows() / nodes_per_dlo;

        // calculate node visibility
        // for each node in Y, determine its shortest distance to X
        // for each point in X, determine its shortest distance to Y
        std::map<int, double> shortest_node_pt_dists;
        std::vector<double> shortest_pt_node_dists(X.rows(), 100000.0);
        for (int m = 0; m < Y.rows(); m ++) {
            int closest_pt_idx = 0;
            double shortest_dist = 100000;
            // loop through all points in X
            for (int n = 0; n < X.rows(); n ++) {
                double dist = (Y.row(m) - X.row(n)).norm();
                // update shortest dist for Y
                if (dist < shortest_dist) {
                    closest_pt_idx = n;
                    shortest_dist = dist;
                }

                // update shortest dist for X
                if (dist < shortest_pt_node_dists[n]) {
                    shortest_pt_node_dists[n] = dist;
                }
            }
            shortest_node_pt_dists.insert(std::pair<int, double>(m, shortest_dist));
        }

        // for current nodes and edges in Y, sort them based on how far away they are from the camera
        std::vector<double> averaged_node_camera_dists = {};
        std::vector<int> indices_vec = {};
        for (int i = 0; i < Y.rows()-1; i ++) {
            averaged_node_camera_dists.push_back(((Y.row(i) + Y.row(i+1)) / 2).norm());
            indices_vec.push_back(i);
        }
        // sort
        std::sort(indices_vec.begin(), indices_vec.end(),
            [&](const int& a, const int& b) {
                return (averaged_node_camera_dists[a] < averaged_node_camera_dists[b]);
            }
        );
        Mat projected_edges = Mat::zeros(mask.rows, mask.cols, CV_8U);

        // project Y^{t-1} onto projected_edges
        MatrixXd Y_h = Y.replicate(1, 1);
        Y_h.conservativeResize(Y_h.rows(), Y_h.cols()+1);
        Y_h.col(Y_h.cols()-1) = MatrixXd::Ones(Y_h.rows(), 1);
        MatrixXd image_coords_mask = (proj_matrix * Y_h.transpose()).transpose();

        std::vector<int> visible_nodes = {};
        std::vector<int> self_occluded_nodes = {};
        std::vector<int> not_self_occluded_nodes = {};
        std::vector<int> self_occluding_nodes = {};

        // draw edges closest to the camera first
        for (int idx : indices_vec) {
            if ((idx + 1) % nodes_per_dlo == 0) {
                continue;
            }

            int col_1 = static_cast<int>(image_coords_mask(idx, 0)/image_coords_mask(idx, 2));
            int row_1 = static_cast<int>(image_coords_mask(idx, 1)/image_coords_mask(idx, 2));

            int col_2 = static_cast<int>(image_coords_mask(idx+1, 0)/image_coords_mask(idx+1, 2));
            int row_2 = static_cast<int>(image_coords_mask(idx+1, 1)/image_coords_mask(idx+1, 2));

            // only add to visible nodes if did not overlap with existing edges
            if (projected_edges.at<uchar>(row_1, col_1) == 0) {
                if (shortest_node_pt_dists[idx] <= visibility_threshold) {
                    if (std::find(visible_nodes.begin(), visible_nodes.end(), idx) == visible_nodes.end()) {
                        visible_nodes.push_back(idx);
                    }
                }
                if (std::find(not_self_occluded_nodes.begin(), not_self_occluded_nodes.end(), idx) == not_self_occluded_nodes.end()) {
                    not_self_occluded_nodes.push_back(idx);
                }
            }

            // do not consider adjacent nodes directly on top of each other
            if (projected_edges.at<uchar>(row_2, col_2) == 0) {
                if (shortest_node_pt_dists[idx+1] <= visibility_threshold) {
                    if (std::find(visible_nodes.begin(), visible_nodes.end(), idx+1) == visible_nodes.end()) {
                        visible_nodes.push_back(idx+1);
                    }
                }
                if (std::find(not_self_occluded_nodes.begin(), not_self_occluded_nodes.end(), idx+1) == not_self_occluded_nodes.end()) {
                    not_self_occluded_nodes.push_back(idx+1);
                }
            }

            // add edges for checking overlap with upcoming nodes
            double x1 = col_1;
            double y1 = row_1;
            double x2 = col_2;
            double y2 = row_2;
            cv::line(projected_edges, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255, 255, 255), dlo_pixel_width);
        }

        // sort visible nodes to preserve the original connectivity
        std::sort(visible_nodes.begin(), visible_nodes.end());

        std::cout << "===== visible nodes =====" << std::endl;
        print_1d_vector(visible_nodes);

        // minor mid-section occlusion is usually fine
        // extend visible nodes so that gaps as small as 2 to 3 nodes are filled
        std::vector<int> visible_nodes_extended = {};
        for (int i = 0; i < visible_nodes.size()-1; i ++) {
            visible_nodes_extended.push_back(visible_nodes[i]);
            // extend visible nodes
            if (fabs(converted_node_coord[visible_nodes[i+1]] - converted_node_coord[visible_nodes[i]]) <= d_vis) {
                // should not extend to nodees on different dlos
                if (int(i / nodes_per_dlo) != int ((i+1) / nodes_per_dlo)) {
                    continue;
                }
                for (int j = 1; j < visible_nodes[i+1] - visible_nodes[i]; j ++) {
                    visible_nodes_extended.push_back(visible_nodes[i] + j);
                }
            }
        }
        visible_nodes_extended.push_back(visible_nodes[visible_nodes.size()-1]);

        // store Y_0 for post processing
        MatrixXd Y_0 = Y.replicate(1, 1);
        
        // step tracker
        multi_dlo_tracker.tracking_step(X, visible_nodes, visible_nodes_extended, proj_matrix, mask.rows, mask.cols);
        Y = multi_dlo_tracker.get_tracking_result();
        guide_nodes = multi_dlo_tracker.get_guide_nodes();
        priors = multi_dlo_tracker.get_correspondence_pairs();

        // post processing
        MatrixXi edges(2, Y.rows());
        edges(0, 0) = 0;
        edges(1, edges.cols() - 1) = Y.rows() - 1;
        for (int i = 1; i <= edges.cols() - 1; ++i) {
            edges(0, i) = i;
            edges(1, i - 1) = i;
        }

        MatrixXi new_edges(2, (nodes_per_dlo - 1) * num_of_dlos);
        int count = 0;
        for (int i = 0; i < num_of_dlos; i ++) {
            for (int j = 0; j < nodes_per_dlo - 1; j ++) {
                new_edges.col(count) = edges.col(i*nodes_per_dlo + j);
                count ++;
            }
        }

        // std::cout << new_edges << std::endl;

        // MatrixXd Y_processed = cdcpd2_post_processing(Y_0.transpose(), Y.transpose(), new_edges, init_nodes.transpose());
        // MatrixXd Y_processed = cdcpd2_post_processing(Y_0.transpose(), Y.transpose(), new_edges);

        // ===== get G =====
        // tracking multiple dlos
        int M = Y_0.rows();
        int kernel = 1;
        double beta_post_proc = 0.1;

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

        G = 1/(2*beta_post_proc * 2*beta_post_proc) * (-sqrt(2)*converted_node_dis/beta_post_proc).array().exp() * (sqrt(2)*converted_node_dis.array() + beta_post_proc);

        if (use_geodesic && num_of_dlos > 1) {
            MatrixXd G_new = MatrixXd::Zero(M, M);
            for (int i = 0; i < num_of_dlos; i ++) {
                int start = i * nodes_per_dlo;
                G_new.block(start, start, nodes_per_dlo, nodes_per_dlo) = G.block(start, start, nodes_per_dlo, nodes_per_dlo);
            }
            G = G_new.replicate(1, 1);
        }

        //post_processing
        MatrixXd Y_processed = post_processing(Y_0.transpose(), Y.transpose(), new_edges, init_nodes.transpose(), G);
        Y = Y_processed.replicate(1, 1);

        // log time
        time_diff = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - cur_time).count() / 1000.0;
        ROS_INFO_STREAM("Tracking step: " + std::to_string(time_diff) + " ms");
        algo_total += time_diff;
        cur_time = std::chrono::high_resolution_clock::now();

        // projection and pub image
        averaged_node_camera_dists = {};
        indices_vec = {};
        for (int i = 0; i < Y.rows()-1; i ++) {
            averaged_node_camera_dists.push_back(((Y.row(i) + Y.row(i+1)) / 2).norm());
            indices_vec.push_back(i);
        }
        // sort
        std::sort(indices_vec.begin(), indices_vec.end(),
            [&](const int& a, const int& b) {
                return (averaged_node_camera_dists[a] < averaged_node_camera_dists[b]);
            }
        );
        std::reverse(indices_vec.begin(), indices_vec.end());

                MatrixXd nodes_h = Y.replicate(1, 1);
        nodes_h.conservativeResize(nodes_h.rows(), nodes_h.cols()+1);
        nodes_h.col(nodes_h.cols()-1) = MatrixXd::Ones(nodes_h.rows(), 1);
        MatrixXd image_coords = (proj_matrix * nodes_h.transpose()).transpose();

        Mat tracking_img;
        tracking_img = 0.5*cur_image_orig + 0.5*cur_image;

        // draw points
        std::vector<std::vector<int>> node_colors = {{255, 0, 0, 255}, {255, 255, 0, 255}, {0, 255, 0, 255}};
        std::vector<std::vector<int>> line_colors = {{255, 0, 0, 255}, {255, 255, 0, 255}, {0, 255, 0, 255}};

        for (int idx : indices_vec) {

            int x = static_cast<int>(image_coords(idx, 0)/image_coords(idx, 2));
            int y = static_cast<int>(image_coords(idx, 1)/image_coords(idx, 2));

            int dlo_index = idx / nodes_per_dlo;

            cv::Scalar point_color = cv::Scalar(node_colors[dlo_index][2], node_colors[dlo_index][1], node_colors[dlo_index][0]);
            cv::Scalar line_color = cv::Scalar(line_colors[dlo_index][2], line_colors[dlo_index][1], line_colors[dlo_index][0]);

            if ((idx+1) % nodes_per_dlo != 0) {
                cv::line(tracking_img, cv::Point(x, y),
                                       cv::Point(static_cast<int>(image_coords(idx+1, 0)/image_coords(idx+1, 2)), 
                                                 static_cast<int>(image_coords(idx+1, 1)/image_coords(idx+1, 2))),
                                       line_color, 5);
            }

            cv::circle(tracking_img, cv::Point(x, y), 7, point_color, -1);

            if ((idx+2) % nodes_per_dlo == 0) {                
                cv::circle(tracking_img, cv::Point(static_cast<int>(image_coords(idx+1, 0)/image_coords(idx+1, 2)), 
                                                   static_cast<int>(image_coords(idx+1, 1)/image_coords(idx+1, 2))),
                                                   7, point_color, -1);
            }
        }

        // add text
        if (updated_opencv_mask && simulated_occlusion) {
            cv::putText(tracking_img, "occlusion", cv::Point(occlusion_corner_j, occlusion_corner_i-10), cv::FONT_HERSHEY_DUPLEX, 1.2, cv::Scalar(0, 0, 240), 2);
        }

        // publish image
        tracking_img_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", tracking_img).toImageMsg();

        // publish the results as a marker array
        // visualization_msgs::MarkerArray results = MatrixXd2MarkerArray(Y, result_frame_id, "node_results", {1.0, 150.0/255.0, 0.0, 1.0}, {0.0, 1.0, 0.0, 1.0}, 0.01, 0.005, visible_nodes, {1.0, 0.0, 0.0, 1.0}, {1.0, 0.0, 0.0, 1.0});
        visualization_msgs::MarkerArray results = MatrixXd2MarkerArray(Y, result_frame_id, "node_results", node_colors, line_colors, 0.006, 0.0035, num_of_dlos, nodes_per_dlo);

        // publish the results as a marker array
        // visualization_msgs::MarkerArray results = MatrixXd2MarkerArray(Y, result_frame_id, "node_results", {1.0, 150.0/255.0, 0.0, 1.0}, {0.0, 1.0, 0.0, 1.0}, 0.01, 0.005, vis, {1.0, 0.0, 0.0, 1.0}, {1.0, 0.0, 0.0, 1.0});
        // // visualization_msgs::MarkerArray results = MatrixXd2MarkerArray(Y, result_frame_id, "node_results", {1.0, 150.0/255.0, 0.0, 1.0}, {0.0, 1.0, 0.0, 1.0}, 0.01, 0.005);
        // visualization_msgs::MarkerArray guide_nodes_results = MatrixXd2MarkerArray(guide_nodes, result_frame_id, "guide_node_results", {0.0, 0.0, 0.0, 0.5}, {0.0, 0.0, 1.0, 0.5});
        // visualization_msgs::MarkerArray corr_priors_results = MatrixXd2MarkerArray(priors, result_frame_id, "corr_prior_results", {0.0, 0.0, 0.0, 0.5}, {1.0, 0.0, 0.0, 0.5});

        // convert to pointcloud2 for eval
        pcl::PointCloud<pcl::PointXYZ> trackdlo_pc;
        for (int i = 0; i < Y.rows(); i++) {
            pcl::PointXYZ temp;
            temp.x = Y(i, 0);
            temp.y = Y(i, 1);
            temp.z = Y(i, 2);
            trackdlo_pc.points.push_back(temp);
        }

        // get self-occluded nodes
        pcl::PointCloud<pcl::PointXYZ> self_occluded_pc;
        for (auto i : self_occluded_nodes) {
            pcl::PointXYZ temp;
            temp.x = Y(i, 0);
            temp.y = Y(i, 1);
            temp.z = Y(i, 2);
            self_occluded_pc.points.push_back(temp);
        }

        // publish filtered point cloud
        pcl::PCLPointCloud2 cur_pc_pointcloud2;
        pcl::PCLPointCloud2 result_pc_poincloud2;
        pcl::PCLPointCloud2 self_occluded_pc_poincloud2;
        pcl::toPCLPointCloud2(cur_pc_downsampled, cur_pc_pointcloud2);
        pcl::toPCLPointCloud2(trackdlo_pc, result_pc_poincloud2);
        pcl::toPCLPointCloud2(self_occluded_pc, self_occluded_pc_poincloud2);

        // Convert to ROS data type
        sensor_msgs::PointCloud2 cur_pc_msg;
        sensor_msgs::PointCloud2 result_pc_msg;
        sensor_msgs::PointCloud2 self_occluded_pc_msg;
        pcl_conversions::moveFromPCL(cur_pc_pointcloud2, cur_pc_msg);
        pcl_conversions::moveFromPCL(result_pc_poincloud2, result_pc_msg);
        pcl_conversions::moveFromPCL(self_occluded_pc_poincloud2, self_occluded_pc_msg);

        // for evaluation sync
        cur_pc_msg.header.frame_id = result_frame_id;
        result_pc_msg.header.frame_id = result_frame_id;
        result_pc_msg.header.stamp = image_msg->header.stamp;
        self_occluded_pc_msg.header.frame_id = result_frame_id;
        self_occluded_pc_msg.header.stamp = image_msg->header.stamp;

        results_pub.publish(results);
        // guide_nodes_pub.publish(guide_nodes_results);
        // corr_priors_pub.publish(corr_priors_results);
        pc_pub.publish(cur_pc_msg);
        result_pc_pub.publish(result_pc_msg);
        self_occluded_pc_pub.publish(self_occluded_pc_msg);

        // // reset all guide nodes
        // for (int i = 0; i < guide_nodes_results.markers.size(); i ++) {
        //     guide_nodes_results.markers[i].action = visualization_msgs::Marker::DELETEALL;
        // }
        // for (int i = 0; i < corr_priors_results.markers.size(); i ++) {
        //     corr_priors_results.markers[i].action = visualization_msgs::Marker::DELETEALL;
        // }

        // log time
        time_diff = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - cur_time).count() / 1000.0;
        ROS_INFO_STREAM("Pub data: " + std::to_string(time_diff) + " ms");
        pub_data_total += time_diff;

        frames += 1;

        ROS_INFO_STREAM("Avg before tracking step: " + std::to_string(pre_proc_total / frames) + " ms");
        ROS_INFO_STREAM("Avg tracking step: " + std::to_string(algo_total / frames) + " ms");
        ROS_INFO_STREAM("Avg pub data: " + std::to_string(pub_data_total / frames) + " ms");
        ROS_INFO_STREAM("Avg total: " + std::to_string((pre_proc_total + algo_total + pub_data_total) / frames) + " ms");
    }
        
    return tracking_img_msg;
}

int main(int argc, char **argv) {
    ros::init(argc, argv, "tracker_node");
    ros::NodeHandle nh;

    // load parameters
    nh.getParam("/multidlo/beta", beta); 
    nh.getParam("/multidlo/lambda", lambda); 
    nh.getParam("/multidlo/alpha", alpha); 
    nh.getParam("/multidlo/lle_weight", lle_weight); 
    nh.getParam("/multidlo/mu", mu); 
    nh.getParam("/multidlo/max_iter", max_iter); 
    nh.getParam("/multidlo/tol", tol);
    nh.getParam("/multidlo/include_lle", include_lle); 
    nh.getParam("/multidlo/use_geodesic", use_geodesic); 
    nh.getParam("/multidlo/use_prev_sigma2", use_prev_sigma2); 

    nh.getParam("/multidlo/multi_color_dlo", multi_color_dlo);
    nh.getParam("/multidlo/nodes_per_dlo", nodes_per_dlo);
    nh.getParam("/multidlo/dlo_diameter", dlo_diameter);
    nh.getParam("/multidlo/check_distance", check_distance);
    nh.getParam("/multidlo/clamp", clamp);
    nh.getParam("/multidlo/downsample_leaf_size", downsample_leaf_size);

    nh.getParam("/multidlo/camera_info_topic", camera_info_topic);
    nh.getParam("/multidlo/rgb_topic", rgb_topic);
    nh.getParam("/multidlo/depth_topic", depth_topic);
    nh.getParam("/multidlo/result_frame_id", result_frame_id);

    nh.getParam("/multidlo/hsv_threshold_upper_limit", hsv_threshold_upper_limit);
    nh.getParam("/multidlo/hsv_threshold_lower_limit", hsv_threshold_lower_limit);

    nh.getParam("/multidlo/visibility_threshold", visibility_threshold);
    nh.getParam("/multidlo/dlo_pixel_width", dlo_pixel_width);
    nh.getParam("/multidlo/d_vis", d_vis);
    nh.getParam("/multidlo/k_vis", k_vis);
    nh.getParam("/multidlo/beta_pre_proc", beta_pre_proc); 
    nh.getParam("/multidlo/lambda_pre_proc", lambda_pre_proc);

    // update color thresholding upper bound
    std::string rgb_val = "";
    for (int i = 0; i < hsv_threshold_upper_limit.length(); i ++) {
        if (hsv_threshold_upper_limit.substr(i, 1) != " ") {
            rgb_val += hsv_threshold_upper_limit.substr(i, 1);
        }
        else {
            upper.push_back(std::stoi(rgb_val));
            rgb_val = "";
        }
        
        if (i == hsv_threshold_upper_limit.length()-1) {
            upper.push_back(std::stoi(rgb_val));
        }
    }

    // update color thresholding lower bound
    rgb_val = "";
    for (int i = 0; i < hsv_threshold_lower_limit.length(); i ++) {
        if (hsv_threshold_lower_limit.substr(i, 1) != " ") {
            rgb_val += hsv_threshold_lower_limit.substr(i, 1);
        }
        else {
            lower.push_back(std::stoi(rgb_val));
            rgb_val = "";
        }
        
        if (i == hsv_threshold_lower_limit.length()-1) {
            upper.push_back(std::stoi(rgb_val));
        }
    }

    int pub_queue_size = 30;

    image_transport::ImageTransport it(nh);
    image_transport::Subscriber opencv_mask_sub = it.subscribe("/mask_with_occlusion", 10, update_opencv_mask);
    init_nodes_sub = nh.subscribe("/init_nodes", 1, update_init_nodes);
    camera_info_sub = nh.subscribe(camera_info_topic, 1, update_camera_info);

    image_transport::Publisher mask_pub = it.advertise("/mask", pub_queue_size);
    image_transport::Publisher tracking_img_pub = it.advertise("/results_img", pub_queue_size);
    pc_pub = nh.advertise<sensor_msgs::PointCloud2>("/filtered_pointcloud", pub_queue_size);
    results_pub = nh.advertise<visualization_msgs::MarkerArray>("/results_marker", pub_queue_size);

    // point cloud topic
    result_pc_pub = nh.advertise<sensor_msgs::PointCloud2>("/results_pc", pub_queue_size);

    message_filters::Subscriber<sensor_msgs::Image> image_sub(nh, rgb_topic, 10);
    message_filters::Subscriber<sensor_msgs::Image> depth_sub(nh, depth_topic, 10);
    message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image> sync(image_sub, depth_sub, 10);

    sync.registerCallback<std::function<void(const sensor_msgs::ImageConstPtr&, 
                                             const sensor_msgs::ImageConstPtr&,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>,
                                             const boost::shared_ptr<const message_filters::NullType>)>>
    (
        [&](const sensor_msgs::ImageConstPtr& img_msg, 
            const sensor_msgs::ImageConstPtr& depth_msg,
            const boost::shared_ptr<const message_filters::NullType> var1,
            const boost::shared_ptr<const message_filters::NullType> var2,
            const boost::shared_ptr<const message_filters::NullType> var3,
            const boost::shared_ptr<const message_filters::NullType> var4,
            const boost::shared_ptr<const message_filters::NullType> var5,
            const boost::shared_ptr<const message_filters::NullType> var6,
            const boost::shared_ptr<const message_filters::NullType> var7)
        {
            sensor_msgs::ImagePtr tracking_img = Callback(img_msg, depth_msg);
            tracking_img_pub.publish(tracking_img);
        }
    );
    
    ros::spin();
}