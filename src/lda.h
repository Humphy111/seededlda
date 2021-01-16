/*
 * Copyright (C) 2007 by
 *
 * 	Xuan-Hieu Phan
 *	hieuxuan@ecei.tohoku.ac.jp or pxhieu@gmail.com
 * 	Graduate School of Information Sciences
 * 	Tohoku University
 *
 * Copyright (C) 2020 by
 *
 * 	Kohei Watanabe
 * 	watanabe.kohei@gmail.com
 *
 * GibbsLDA++ is a free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * GibbsLDA++ is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GibbsLDA++; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

/*
 * References:
 * + The Java code of Gregor Heinrich (gregor@arbylon.net)
 *   http://www.arbylon.net/projects/LdaGibbsSampler.java
 * + "Parameter estimation for text analysis" by Gregor Heinrich
 *   http://www.arbylon.net/publications/text-est.pdf
 */

#include "lib.h"
#include "dev.h"

using namespace std;
using namespace Rcpp;
using namespace quanteda;

// LDA model
class LDA {
    public:
        // --- model parameters and variables ---
        int M; // dataset size (i.e., number of docs)
        int V; // vocabulary size
        int K; // number of topics
        double alpha, beta; // LDA hyperparameters
        int niters; // number of Gibbs sampling iterations
        int liter; // the iteration at which the model was saved
        int random; // seed for random number generation
        bool verbose; // print progress messages

        arma::sp_mat data; // transposed document-feature matrix
        Texts topics; // topic assignments for words, size M x doc.size()
        Texts texts;
        arma::mat n_word_topic; // word_topic[i][j]: number of instances of word/term i assigned to topic j, size V x K
        arma::mat n_doc_topic; // doc_topic[i][j]: number of words in document i assigned to topic j, size M x K
        arma::colvec s_word_topic; // s_word_topic[j]: total number of words assigned to topic j, size K
        arma::colvec s_doc_topic; // s_doc_topic[i]: total number of words in document i, size M
        arma::mat theta; // theta: document-topic distributions, size M x K
        arma::mat phi; // phi: topic-word distributions, size K x V

        // prediction with fitted model
        arma::mat n_word_topic_fit;
        arma::colvec s_word_topic_fit;

        // random number generators
        std::default_random_engine generator;
        std::uniform_real_distribution<double> random_prob;
        std::uniform_int_distribution<int> random_topic;

        // --------------------------------------

        LDA() {
    	    set_default_values();
        }

        // set default values for variables
        void set_default_values();
        void set_data(arma::sp_mat mt);
        void set_fitted(arma::sp_mat mt);

        // init for estimation
        int init_est();

        // estimate LDA model using Gibbs sampling
        void fit();
        void estimate(int m);
        int sample(int m, int n,
                   arma::mat& n_wt, arma::mat& n_dt,
                   arma::colvec& s_wt, arma::colvec& s_dt);
        void compute_theta();
        void compute_phi();

};

void LDA::set_default_values() {

    M = 0;
    V = 0;
    K = 100;
    alpha = 50.0 / K;
    beta = 0.1;
    niters = 2000;
    liter = 0;
    verbose = false;
    random = 1234;
}

void LDA::set_data(arma::sp_mat mt) {

    //Rprintf("Set data\n");
    data = mt.t();
    M = data.n_cols;
    V = data.n_rows;
    //printf("M = %d, V = %d\n", M, V);
}

void LDA::set_fitted(arma::sp_mat words) {

    //Rprintf("Set fitted LDA\n");
    if ((int)words.n_rows != V || (int)words.n_cols != K)
        throw std::invalid_argument("Invalid word matrix");
    n_word_topic_fit = arma::mat(words.t());
    s_word_topic_fit = arma::sum(words, 0).t();

}

int LDA::init_est() {

    if (verbose) {
        Rprintf("Fitting LDA with %d topics\n", K);
        Rprintf("   ...initializing\n");
    }
    std::default_random_engine generator(random);
    std::uniform_real_distribution< double > random_prob(0, 1);
    std::uniform_int_distribution< int > random_topic(0, K - 1);

    topics = Texts(M);
    texts = Texts(M);

    theta = arma::mat(M, K, arma::fill::zeros);
    phi = arma::mat(K, V, arma::fill::zeros);
    n_word_topic = arma::mat(K, V, arma::fill::zeros);
    n_doc_topic = arma::mat(K, M, arma::fill::zeros);
    s_word_topic = arma::colvec(K, arma::fill::zeros);
    //s_doc_topic = arma::mat(arma::sum(data, 0)).t();
    s_doc_topic = arma::sum(data, 0).t();
    //s_doc_topic = arma::mat(arma::sum(data, 1));
    //dev::Timer timer;
    //dev::start_timer("Set z", timer);
    for (int m = 0; m < M; m++) {

        topics[m] = Text(s_doc_topic[m]);
        texts[m] = Text(s_doc_topic[m]);

        if (texts[m].size() == 0) continue;
        int n = 0;

        arma::sp_mat::const_col_iterator it = data.begin_col(m);
        arma::sp_mat::const_col_iterator it_end = data.end_col(m);
        for(; it != it_end; ++it) {
            int w = it.row();
            int F = *it;
            for (int f = 0; f < F; f++) {
                int topic = random_topic(generator);
                topics[m][n] = topic;
                texts[m][n] = w;
                // number of instances of word i assigned to topic j
                n_word_topic.at(topic, w) += 1;
                // number of words in document i assigned to topic j
                n_doc_topic.at(topic, m) += 1;
                // total number of words assigned to topic j
                s_word_topic[topic] += 1;
                n++;
            }
        }
    }
    //dev::stop_timer("Set z", timer);
    return 0;
}

void LDA::fit() {

    if (verbose)
        Rprintf("   ...Gibbs sampling in %d itterations\n", niters);

    int last_iter = liter;
    for (liter = last_iter + 1; liter <= niters + last_iter; liter++) {

        if (liter % 100 == 0) {
            checkUserInterrupt();
            if (verbose)
                Rprintf("   ...iteration %d\n", liter);
        }

        // for all z_i
        for (int m = 0; m < M; m++) {
            estimate(m);
        }
    }

    if (verbose)
        Rprintf("   ...computing theta and phi\n");
    //compute_theta();
    //compute_phi();
    liter--;
    if (verbose)
        Rprintf("   ...complete\n");
}

void LDA::estimate(int m) {

    // object for the local distribution
    //arma::mat n_wt = arma::mat(K, V, arma::fill::zeros);
    //arma::mat n_dt = arma::mat(K, M, arma::fill::zeros);
    //arma::colvec s_wt = arma::colvec(K, arma::fill::zeros);
    //arma::colvec s_dt = arma::colvec(M, arma::fill::zeros);

    arma::mat n_wt, n_wt_init, n_dt, n_dt_init;
    n_wt = n_wt_init = n_word_topic + n_word_topic_fit;
    n_dt = n_dt_init = n_doc_topic;

    arma::colvec s_wt, s_wt_init, s_dt, s_dt_init;
    s_wt = s_wt_init = s_word_topic + s_word_topic_fit;
    s_dt = s_dt_init = s_doc_topic;

    if (texts[m].size() == 0) return;
    for (int n = 0; n < texts[m].size(); n++) {
        topics[m][n] = sample(m, n, n_wt, n_dt, s_wt, s_dt);
    }

    // updated the global distribution
    n_word_topic.col(m) += n_wt.col(m) - n_wt_init.col(m);
    n_doc_topic.col(m) += n_dt.col(m) - n_dt_init.col(m);
    s_word_topic += s_wt - s_wt_init;
    s_doc_topic += s_dt - s_dt_init;
}


int LDA::sample(int m, int n,
                arma::mat& n_wt, arma::mat& n_dt,
                arma::colvec& s_wt, arma::colvec& s_dt) {

    // remove z_i from the count variables
    int w = texts[m][n];
    int topic = topics[m][n];
    n_wt.at(topic, w) -= 1;
    n_dt.at(topic, m) -= 1;
    s_wt[topic] -= 1;
    s_dt[m] -= 1;

    // do multinomial sampling via cumulative method

    // arma::colvec n_wt_all = n_word_topic.col(w) + n_wt.col(w); + n_word_topic_fit.col(w);
    // arma::colvec s_wt_all = s_word_topic + s_wt + s_word_topic_fit;
    // arma::colvec n_dt_all = n_doc_topic.col(m) + n_dt.col(m);
    // double s_dt_all = s_doc_topic[m] + s_dt[m];
    // arma::colvec p = ((n_wt_all + beta) / (s_wt_all + V * beta)) % ((n_dt_all + alpha) / (s_dt_all + K * alpha));

    double Vbeta = V * beta;
    double Kalpha = K * alpha;
    std::vector< double > p(K);
    for (int k = 0; k < K; k++) {
        p[k] = ((n_wt.at(k, w) + beta) / (s_wt[k] + Vbeta)) * ((n_dt.at(k, m) + alpha) / (s_dt[m] + Kalpha));
    }

    // double Vbeta = V * beta;
    // double Kalpha = K * alpha;
    // std::vector< double > p(K);
    // for (int k = 0; k < K; k++) {
    //     p[k] = (n_word_topic.at(k, w) + n_wt.at(k, w) + n_word_topic_fit.at(k, w) + beta) / (s_word_topic[k] + s_wt[k] + s_word_topic_fit[k] + Vbeta) *
    //            (n_doc_topic.at(k, m) + n_dt.at(k, m) + alpha) / (s_doc_topic[m] + s_dt[m] + Kalpha);
    //     //p[k] = (n_word_topic.at(k, w) + n_wt.at(k, w) + beta) / (s_word_topic[k] + s_wt[k] + Vbeta) *
    //     //       (n_doc_topic.at(k, m) + n_dt.at(k, m) + alpha) / (s_doc_topic[m] + s_dt[m] + Kalpha);
    // }
    // cumulate multinomial parameters
    for (int k = 1; k < K; k++) {
        p[k] += p[k - 1];
    }
    // scaled sample because of unnormalized p[]
    double u = random_prob(generator) * p[K - 1];

    // rejection sampling
    for (int k = 0; k < K; k++) {
        topic = k;
        if (p[k] > u) {
            break;
        }
    }

    // add newly estimated z_i to count variables
    n_wt.at(topic, w) += 1;
    n_dt.at(topic, m) += 1;
    s_wt[topic] += 1;
    s_dt[m] += 1;

    return topic;
}

void LDA::compute_theta() {
    for (int m = 0; m < M; m++) {
        for (int k = 0; k < K; k++) {
            theta.at(m, k) = (n_doc_topic.at(k, m) + alpha) / (s_doc_topic[m] + K * alpha);
        }
    }
}

void LDA::compute_phi() {
    for (int k = 0; k < K; k++) {
        for (int w = 0; w < V; w++) {
            phi.at(k, w) = (n_word_topic.at(k, w) + n_word_topic_fit.at(k, w) + beta) / (s_word_topic[k] + s_word_topic_fit[k] + V * beta);
        }
    }
}
