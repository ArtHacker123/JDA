#include <cmath>
#include <climits>
#include <opencv2/imgproc/imgproc.hpp>
#include "jda/data.hpp"
#include "jda/cart.hpp"
#include "jda/common.hpp"
#include "jda/cascador.hpp"

using namespace cv;
using namespace std;

namespace jda {

Cart::Cart() {}
Cart::~Cart() {}
void Cart::Initialize(int stage, int landmark_id) {
    const Config& c = Config::GetInstance();
    this->stage = stage;
    this->landmark_id = landmark_id;
    depth = c.tree_depth;
    leafNum = 1 << (depth - 1);
    nodes_n = 1 << depth;
    featNum = c.feats[stage];
    radius = c.radius[stage];
    p = c.probs[stage];
    features.resize(nodes_n / 2);
    thresholds.resize(nodes_n / 2);
    scores.resize(nodes_n / 2);
}

void Cart::Train(DataSet& pos, DataSet& neg) {
    vector<int> pos_idx, neg_idx;
    int n = pos.size;
    pos_idx.resize(n);
    for (int i = 0; i < n; i++) pos_idx[i] = i;
    n = neg.size;
    neg_idx.resize(n);
    for (int i = 0; i < n; i++) neg_idx[i] = i;
    // split node from root with idx = 1, why 1? see binary tree in sequence
    SplitNode(pos, pos_idx, neg, neg_idx, 1);
}

void Cart::SplitNode(DataSet& pos, vector<int>& pos_idx, \
                     DataSet& neg, vector<int>& neg_idx, \
                     int node_idx) {
    const Config& c = Config::GetInstance();
    const int pos_n = pos_idx.size();
    const int neg_n = neg_idx.size();
    if (node_idx >= nodes_n / 2) {
        // we are on a leaf node
        const int idx = node_idx - nodes_n / 2;
        double pos_w, neg_w;
        pos_w = neg_w = c.esp;
        for (int i = 0; i < pos_n; i++) {
            pos_w += pos.weights[pos_idx[i]];
        }
        for (int i = 0; i < neg_n; i++) {
            neg_w += neg.weights[neg_idx[i]];
        }
        scores[idx] = 0.5 * log(pos_w / neg_w);
        return;
    }

    // feature pool
    vector<Feature> feature_pool;
    Mat_<int> pos_feature, neg_feature;
    GenFeaturePool(feature_pool);
    pos_feature = pos.CalcFeatureValues(feature_pool, pos_idx);
    neg_feature = neg.CalcFeatureValues(feature_pool, neg_idx);
    // classification or regression
    RNG rng(getTickCount());
    bool is_classification = (rng.uniform(0., 1.) < p) ? true : false;
    int feature_idx, threshold;
    if (is_classification) {
        LOG("\tSplit %d th node by Classification", node_idx);
        SplitNodeWithClassification(pos, pos_idx, neg, neg_idx, \
                                    pos_feature, neg_feature, \
                                    feature_idx, threshold);
    }
    else {
        LOG("\tSplit %d th node by Regression", node_idx);
        Mat_<double> shape_residual = pos.CalcShapeResidual(pos_idx, landmark_id);
        SplitNodeWithRegression(pos, pos_idx, neg, neg_idx, \
                                pos_feature, shape_residual, \
                                feature_idx, threshold);
    }
    // split training data into left and right if any more
    vector<int> left_pos_idx, left_neg_idx;
    vector<int> right_pos_idx, right_neg_idx;
    left_pos_idx.reserve(pos_n);
    right_pos_idx.reserve(pos_n);
    left_neg_idx.reserve(neg_n);
    right_neg_idx.reserve(neg_n);

    for (int i = 0; i < pos_n; i++) {
        if (pos_feature(feature_idx, i) <= threshold) {
            left_pos_idx.push_back(pos_idx[i]);
        }
        else {
            right_pos_idx.push_back(pos_idx[i]);
        }
    }
    for (int i = 0; i < neg_n; i++) {
        if (neg_feature(feature_idx, i) <= threshold) {
            left_neg_idx.push_back(neg_idx[i]);
        }
        else {
            right_neg_idx.push_back(neg_idx[i]);
        }
    }
    // save parameters on this node
    features[node_idx] = feature_pool[feature_idx];
    thresholds[node_idx] = threshold;
    // split node in DFS way
    SplitNode(pos, left_pos_idx, neg, left_neg_idx, 2 * node_idx);
    SplitNode(pos, right_pos_idx, neg, right_neg_idx, 2 * node_idx + 1);
}

/**
 * Calculate Gini `gini = 2*p*(1-p)`
 * :input p:        p
 * :return gini:    gini
 */
static inline double calcGini(double p) {
    double gini = 2 * p * (1 - p);
    return gini;
}

void Cart::SplitNodeWithClassification(DataSet& pos, const vector<int>& pos_idx, \
                                       DataSet& neg, const vector<int>& neg_idx, \
                                       const Mat_<int>& pos_feature, \
                                       const Mat_<int>& neg_feature, \
                                       int& feature_idx, int& threshold) {
    const Config& c = Config::GetInstance();
    const int feature_n = pos_feature.rows;
    const int pos_n = pos_feature.cols;
    const int neg_n = neg_feature.cols;
    RNG rng(getTickCount());
    feature_idx = 0;
    threshold = -256; // all data will go to right child tree

    // select a feature that has minimum gini
    vector<double> gs_(feature_n);
    vector<int> ths_(feature_n);
    
    #pragma omp parallel for
    for (int i = 0; i < feature_n; i++) {
        double wp_l, wp_r, wn_l, wn_r, w;
        wp_l = wp_r = wn_l = wn_r = w = 0;
        vector<double> wp(511, 0), wn(511, 0);
        for (int j = 0; j < pos_n; j++) {
            wp[pos_feature(i, j) + 255] += pos.weights[pos_idx[j]];
            wp_r += pos.weights[pos_idx[j]];
        }
        for (int j = 0; j < neg_n; j++) {
            wn[neg_feature(i, j) + 255] += neg.weights[neg_idx[j]];
            wn_r += neg.weights[neg_idx[j]];
        }
        w = wp_r + wn_r;

        int threshold_ = -256;
        double gini = calcGini(wp_r / w);
        for (int th = -255; th <= 255; th++) {
            const int idx = th + 255;
            wp_l += wp[idx];
            wn_l += wn[idx];
            wp_r -= wp[idx];
            wn_r -= wn[idx];
            double w_l = wp_l + wn_l + c.esp;
            double w_r = wp_r + wn_r + c.esp;
            double g = (w_l / w) * calcGini(wp_l / w_l) + \
                       (w_r / w) * calcGini(wp_r / w_r);
            if (g < gini) {
                gini = g;
                threshold_ = th;
            }
        }
        gs_[i] = gini;
        ths_[i] = threshold_;
    }

    double gini_min = numeric_limits<double>::max();
    for (int i = 0; i < feature_n; i++) {
        if (gs_[i] < gini_min) {
            gini_min = gs_[i];
            threshold = ths_[i];
            feature_idx = i;
        }
    }
    // Done
}

/**
* Calculate Variance of vector
*/
double calcVariance(const Mat_<double>& vec) {
    double m1 = cv::mean(vec)[0];
    double m2 = cv::mean(vec.mul(vec))[0];
    double variance = m2 - m1*m1;
    return variance;
}
double calcVariance(const vector<double>& vec) {
    if (vec.size() == 0) return 0.;
    Mat_<double> vec_(vec);
    double m1 = cv::mean(vec_)[0];
    double m2 = cv::mean(vec_.mul(vec_))[0];
    double variance = m2 - m1*m1;
    return variance;
}

void Cart::SplitNodeWithRegression(DataSet& pos, const std::vector<int>& pos_idx, \
                                   DataSet& neg, const std::vector<int>& neg_idx, \
                                   const Mat_<int>& pos_feature, \
                                   const Mat_<double>& shape_residual, \
                                   int& feature_idx, int& threshold) {
    const int feature_n = pos_feature.rows;
    const int pos_n = pos_feature.cols;
    Mat_<int> pos_feature_sorted;
    cv::sort(pos_feature, pos_feature_sorted, SORT_EVERY_ROW + SORT_ASCENDING);
    // total variance
    double variance_all = (calcVariance(shape_residual.col(0)) + \
                           calcVariance(shape_residual.col(1))) * pos_n;
    RNG rng(getTickCount());
    feature_idx = 0;
    threshold = -256; // all data will go to right child tree

    if (pos_n == 0) {
        return;
    }

    double variance_reduce_max = 0;
    // select a feature reduce maximum variance
    vector<double> vs_(feature_n);
    vector<int> ths_(feature_n);

    #pragma omp parallel for
    for (int i = 0; i < feature_n; i++) {
        vector<double> left_x, left_y, right_x, right_y;
        left_x.reserve(pos_n); left_y.reserve(pos_n);
        right_x.reserve(pos_n); right_y.reserve(pos_n);
        int threshold_ = pos_feature_sorted(i, static_cast<int>(pos_n*rng.uniform(0.05, 0.95)));
        for (int j = 0; j < pos_n; j++) {
            if (pos_feature(i, j) <= threshold_) {
                left_x.push_back(shape_residual(j, 0));
                left_y.push_back(shape_residual(j, 1));
            }
            else {
                right_x.push_back(shape_residual(j, 0));
                right_y.push_back(shape_residual(j, 1));
            }
        }
        double variance_ = (calcVariance(left_x) + calcVariance(left_y))*left_x.size() + \
                           (calcVariance(right_x) + calcVariance(right_y))*right_x.size();
        double variance_reduce = variance_all - variance_;
        vs_[i] = variance_reduce;
        ths_[i] = threshold_;
    }

    for (int i = 0; i < feature_n; i++) {
        if (vs_[i] > variance_reduce_max) {
            variance_reduce_max = vs_[i];
            threshold = ths_[i];
            feature_idx = i;
        }
    }
    // Done
}

void Cart::GenFeaturePool(vector<Feature>& feature_pool) {
    const Config& c = Config::GetInstance();
    const int landmark_n = c.landmark_n;
    RNG rng(getTickCount());
    feature_pool.resize(featNum);
    for (int i = 0; i < featNum; i++) {
        double x1, y1, x2, y2;
        x1 = rng.uniform(-1., 1.); y1 = rng.uniform(-1., 1.);
        x2 = rng.uniform(-1., 1.); y2 = rng.uniform(-1., 1.);
        // needs to be in a circle
        if (x1*x1 + y1*y1 > 1. || x2*x2 + y2*y2 > 1.) {
            i--;
            continue;
        }
        Feature& feat = feature_pool[i];
        switch (rng.uniform(0, 3)) {
        case 0:
            feat.scale = Feature::ORIGIN; break;
        case 1:
            feat.scale = Feature::HALF; break;
        case 2:
            feat.scale = Feature::QUARTER; break;
        default:
            feat.scale = Feature::ORIGIN; break;
        }
        feat.landmark_id1 = rng.uniform(0, landmark_n);
        feat.landmark_id2 = rng.uniform(0, landmark_n);
        feat.offset1_x = x1*radius;
        feat.offset1_y = y1*radius;
        feat.offset2_x = x2*radius;
        feat.offset2_y = y2*radius;
    }
}

int Cart::Forward(const Mat& img, const Mat& img_h, const Mat& img_q, \
                  const Mat_<double>& shape) const {
    const Config& c = Config::GetInstance();
    int node_idx = 1;
    int len = depth - 1;
    while (len--) {
        const Feature& feature = features[node_idx];
        int val = feature.CalcFeatureValue(img, img_h, img_q, shape);
        if (val <= thresholds[node_idx]) node_idx = 2 * node_idx;
        else node_idx = 2 * node_idx + 1;
    }
    const int bias = 1 << (depth - 1);
    return node_idx - bias;
}

} // namespace jda
