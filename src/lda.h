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
#include <chrono>

using namespace std;
//using namespace Rcpp;
using namespace quanteda;

// Matrix-like object
class Array {

public:
    std::size_t row, col;
    typedef std::vector<int> Row;
    typedef std::vector<Row> Data;
    Data data;

    // constructors
    Array(): row(0), col(0), data(0, std::vector<int>(0, 0)) {} // empty
    Array(std::size_t n): row(1), col(n), data(1, std::vector<int>(n, 0)) {} // vector
    Array(std::size_t r, std::size_t c): row(r), col(c), data(r, std::vector<int>(c, 0)) {} // matrix
    Array(arma::mat x): row(x.n_rows), col(x.n_cols), data(convert(x)) {}

    // convert from arma::mat
    Data convert(arma::mat x) {
        Data temp(x.n_rows, std::vector<int>(x.n_cols, 0));
        for (std::size_t i = 0;  i < x.n_rows; i++) {
            for (std::size_t j = 0;  j < x.n_cols; j++) {
                temp[i][j] = x.at(i, j);
            }
        }
        return temp;
    }

    // allow access by .at()
    int & at(int i, int j) {
        return data[i][j];
    }
    int & at(int j) {
        return data[0][j];
    }

    // allow access by [i][j]
    Row & operator[](std::size_t i) {
        return data[i];
    }

    // allow addition by +=
    Array& operator+=(const Array &arr) {
        if (this->row != arr.data.size())
            throw std::range_error("Invalid number of rows");
        for (std::size_t i = 0; i < this->data.size(); i++) {
            if (this->col != arr.data[i].size())
                throw std::range_error("Invalid number of colmuns");
            for (std::size_t j = 0; j < this->data[i].size(); j++) {
                this->data[i][j] += arr.data[i][j];
            }
        }
        return *this;
    }
};

// LDA model
class LDA {
    public:
        // --- model parameters and variables ---
        int M; // dataset size (i.e., number of docs)
        int V; // vocabulary size
        int K; // number of topics
        double alpha, beta, Vbeta, Kalpha; // parameters for smoothing
        int max_iter; // number of Gibbs sampling iterations
        int iter; // the iteration at which the model was saved
        int random; // seed for random number generation
        int batch; // size of subsets to distribute
        bool verbose; // print progress messages
        int thread; // numebr of thread in parallel processing

        // topic transition
        double gamma; // parameter for topic transition
        std::vector<bool> first; // first[i], documents i are first sentence, size M
        arma::vec q; // temp variable for previous document

        arma::sp_mat data; // transposed document-feature matrix
        Texts texts; // individual words
        Texts z; // topic assignments for words, size M x doc.size()
        Array nw; // nw[i][j]: number of instances of word/term i assigned to topic j, size V x K
        Array nd; // nd[i][j]: number of words in document i assigned to topic j, size M x K
        Array nwsum; // nwsum[j]: total number of words assigned to topic j, size K
        Array ndsum; // nasum[i]: total number of words in document i, size M
        // arma::mat nw; // nw[i][j]: number of instances of word/term i assigned to topic j, size V x K
        // arma::mat nd; // nd[i][j]: number of words in document i assigned to topic j, size M x K
        // arma::rowvec nwsum; // nwsum[j]: total number of words assigned to topic j, size K
        // arma::colvec ndsum; // nasum[i]: total number of words in document i, size M

        arma::mat theta; // theta: document-topic distributions, size M x K
        arma::mat phi; // phi: topic-word distributions, size K x V

        // prediction with fitted model
        Array nw_ft;
        Array nwsum_ft;

        // random number generators
        std::default_random_engine generator;
        std::uniform_real_distribution<double> random_prob;
        std::uniform_int_distribution<int> random_topic;

        // --------------------------------------

        // constructor
        LDA(int K, double alpha, double beta, double gamma, int max_iter,
            int random, int batch, bool verbose, int thread);

        // set default values for variables
        void set_default_values();
        void set_data(arma::sp_mat mt, std::vector<bool> first);
        void set_fitted(arma::sp_mat mt);

        // init for estimation
        int init_est();

        // estimate LDA model using Gibbs sampling
        void estimate();
        int sampling(int m, int n, int w, Array &nw_tp, Array &nwsum_tp);
        void compute_theta();
        void compute_phi();

};

LDA::LDA(int K, double alpha, double beta, double gamma, int max_iter,
         int random, int batch, bool verbose, int thread) {

    set_default_values();
    this->K = K;
    if (0 < alpha)
        this->alpha = alpha;
    if (0 < beta)
        this->beta = beta;
    if (0 < gamma)
        this->gamma = gamma;
    if (0 < max_iter)
        this->max_iter = max_iter;
    if (0 < thread && thread <= tbb::this_task_arena::max_concurrency())
        this->thread = thread;
    this->random = random;
    this->batch = batch;
    this->verbose = verbose;

}

void LDA::set_default_values() {

    M = 0;
    V = 0;
    K = 100;
    alpha = 0.5;
    beta = 0.1;
    max_iter = 2000;
    iter = 0;
    verbose = false;
    random = 1234;
    gamma = 0;
    first = std::vector<bool>(M);
    thread = tbb::this_task_arena::max_concurrency();

}

void LDA::set_data(arma::sp_mat mt, std::vector<bool> first) {

    data = mt.t();
    M = data.n_cols;
    V = data.n_rows;
    this->first = first;

    //printf("M = %d, V = %d\n", M, V);
}

void LDA::set_fitted(arma::sp_mat words) {

    if ((int)words.n_rows != V || (int)words.n_cols != K)
        throw std::invalid_argument("Invalid word matrix");

    nw_ft = Array(arma::mat(words));
    //nw_ft = arma::conv_to<arma::mat>::from(arma::mat(words));
    nwsum_ft = Array(arma::mat(arma::sum(words, 0)));
    //nwsum_ft = arma::sum(nw_ft, 0);

}

int LDA::init_est() {

    if (verbose) {
        Rprintf("Fitting LDA with %d topics\n", K);
        Rprintf(" ...initializing\n");
    }

    std::default_random_engine generator(random);
    std::uniform_real_distribution< double > random_prob(0, 1);
    std::uniform_int_distribution< int > random_topic(0, K - 1);

    theta = arma::mat(M, K, arma::fill::zeros);
    phi = arma::mat(K, V, arma::fill::zeros);

    Array nw(V, K);
    Array nd(M, K);
    //nw = arma::mat(V, K, arma::fill::zeros);
    //nd = arma::mat(M, K, arma::fill::zeros);
    Array nwsum(K);
    Array ndsum(arma::mat(arma::sum(data, 0)));
    //ndsum = arma::conv_to< std::vector<int> >::from(arma::mat(arma::sum(data, 0)));
    q = arma::vec(K);
    Vbeta = V * beta;
    Kalpha = K * alpha;

    // initialize z and texts
    z = Texts(M);
    texts = Texts(M);
    for (int m = 0; m < M; m++) {
        z[m] = Text(ndsum.at(m));
        texts[m] = Text(ndsum.at(m));
        arma::sp_mat::const_col_iterator it = data.begin_col(m);
        arma::sp_mat::const_col_iterator it_end = data.end_col(m);
        int i = 0;
        for(; it != it_end; ++it) {
            int w = it.row();
            int F = *it;
            for (int f = 0; f < F; f++) {
                texts[m][i] = w;
                i++;
            }
        }
    }

    tbb::task_arena arena(thread);
    arena.execute([&]{
        tbb::parallel_for(tbb::blocked_range<int>(0, M, batch), [&](tbb::blocked_range<int> r) {
            //for (int m = 0; m < M; m++) {
            for (int m = r.begin(); m < r.end(); ++m) {
                if (texts[m].size() == 0) continue;
                for (std::size_t i = 0; i < texts[m].size(); i++) {
                    int topic = random_topic(generator);
                    int w = texts[m][i];
                    z[m][i] = topic;
                    // number of instances of word i assigned to topic j
                    nw.at(w, topic) += 1;
                    // number of words in document i assigned to topic j
                    nd.at(m, topic) += 1;
                    // total number of words assigned to topic j
                    nwsum.at(topic) += 1;
                    //nwsum[0][topic] += 1;
                }
            }
        }, tbb::auto_partitioner());
        //dev::stop_timer("Set z", timer);
    });
    return 0;
}

void LDA::estimate() {

    if (verbose && thread > 1 && batch != M) {
        Rprintf(" ...using up to %d threads for distributed computing\n", thread);
        Rprintf(" ...allocating %d documents to each thread\n", batch);
    }
    if (verbose)
        Rprintf(" ...Gibbs sampling in %d itterations\n", max_iter);

    int iter_inc = 100;
    int last_iter = iter;
    for (iter = last_iter + iter_inc; iter <= max_iter + last_iter; iter += iter_inc) {

        auto start = std::chrono::high_resolution_clock::now();
        checkUserInterrupt();
        if (verbose)
            Rprintf(" ...iteration %d", iter);

        Mutex mutex;
        //dev::Timer timer;
        //dev::start_timer("Process batch", timer);
        tbb::task_arena arena(thread);
        arena.execute([&]{
            tbb::parallel_for(tbb::blocked_range<int>(0, M, batch), [&](tbb::blocked_range<int> r) {

                Array nw_tp(V, K);
                //arma::mat nw_tp = arma::mat(size(nw), arma::fill::zeros);
                Array nwsum_tp(K);
                //arma::colvec nwsum_tp = arma::colvec(size(nwsum), arma::fill::zeros);
                //dev::start_timer("Iter", timer);
                for (int i = 0; i < iter_inc; i++) {
                    for (int m = r.begin(); m < r.end(); ++m) {

                        // topic of the previous document
                        for (int k = 0; k < K; k++) {
                            if (gamma == 0 || first[m] || m == 0) {
                                q[k] = 1.0;
                            } else {
                                q[k] = pow((nd.at(m - 1, k) + alpha) / (ndsum.at(m - 1) + K * alpha), gamma);
                                //q[k] = pow((nd.at(m - 1, k) + alpha) / (ndsum[m - 1] + K * alpha), gamma);
                            }
                        }
                        if (texts[m].size() == 0) continue;
                        for (std::size_t i = 0; i < texts[m].size(); i++) {
                            int w = texts[m][i];
                            z[m][i] = sampling(m, i, w, nw_tp, nwsum_tp);
                        }
                    }
                }
                mutex.lock();
                //dev::stop_timer("Iter", timer);
                nw += nw_tp;
                nwsum += nwsum_tp;
                mutex.unlock();
            }, tbb::auto_partitioner());
            //dev::stop_timer("Process batch", timer);
        });
        auto now = std::chrono::high_resolution_clock::now();
        auto sec = std::chrono::duration<double, std::milli>(now - start);
        if (verbose)
            Rcout << " finished in " << (sec.count() / 1000) << " seconds\n";
    }

    if (verbose)
        Rprintf(" ...computing theta and phi\n");
    if (verbose)
        Rprintf(" ...complete\n");
}

int LDA::sampling(int m, int i, int w,
                  Array &nw_tp,
                  Array &nwsum_tp) {

    // remove z_i from the count variables
    int topic = z[m][i];
    nw_tp.at(w, topic) -= 1;
    nwsum_tp.at(topic) -= 1;
    //nwsum_tp[topic] -= 1;
    nd.at(m, topic) -= 1;
    std::vector<double> p(K);
    //arma::vec p(K);

    // do multinomial sampling via cumulative method
    for (int k = 0; k < K; k++) {
        p[k] = ((nw.at(w, k) + nw_tp.at(w, k) + nw_ft.at(w, k) + beta) /
                (nwsum.at(k) + nwsum_tp.at(k) + nwsum_ft.at(k) + Vbeta)) *
                ((nd.at(m, k) + alpha) / (ndsum.at(m) + Kalpha)) * q[k];
        // p[k] = ((nw.at(w, k) + nw_tp.at(w, k) + nw_ft.at(w, k) + beta) /
        //         (nwsum[k] + nwsum_tp[k]+ nwsum_ft[k] + Vbeta)) *
        //        ((nd.at(m, k) + alpha) / (ndsum[m] + Kalpha)) * q[k];

    }
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
    nw_tp.at(w, topic) += 1;
    nwsum_tp.at(topic) += 1;
    nd.at(m, topic) += 1;

    return topic;
}

void LDA::compute_theta() {
    for (int m = 0; m < M; m++) {
        for (int k = 0; k < K; k++) {
            theta.at(m, k) = (nd.at(m, k) + alpha) / (ndsum.at(m) + K * alpha);
        }
    }
}

void LDA::compute_phi() {
    for (int k = 0; k < K; k++) {
        for (int w = 0; w < V; w++) {
            phi.at(k, w) = (nw.at(w, k) + nw_ft.at(w, k) + beta) / (nwsum.at(k) + nwsum_ft.at(k) + V * beta);
        }
    }
}
