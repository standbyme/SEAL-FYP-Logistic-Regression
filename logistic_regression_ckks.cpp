#include <iostream>
#include <iomanip>
#include <fstream>
#include "seal/seal.h"

using namespace std;
using namespace seal;

#define POLY_MOD_DEGREE 16384
#define DEGREE 3
#define ITERS 10
#define LEARNING_RATE 0.1

// Helper function that prints parameters
void print_parameters(shared_ptr<SEALContext> context)
{
    // Verify parameters
    if (!context)
    {
        throw invalid_argument("context is not set");
    }
    auto &context_data = *context->key_context_data();

    string scheme_name;
    switch (context_data.parms().scheme())
    {
    case scheme_type::BFV:
        scheme_name = "BFV";
        break;
    case scheme_type::CKKS:
        scheme_name = "CKKS";
        break;
    default:
        throw invalid_argument("unsupported scheme");
    }
    cout << "/" << endl;
    cout << "| Encryption parameters :" << endl;
    cout << "|   scheme: " << scheme_name << endl;
    cout << "|   poly_modulus_degree: " << context_data.parms().poly_modulus_degree() << endl;

    cout << "|   coeff_modulus size: ";
    cout << context_data.total_coeff_modulus_bit_count() << " (";
    auto coeff_modulus = context_data.parms().coeff_modulus();
    size_t coeff_mod_count = coeff_modulus.size();
    for (size_t i = 0; i < coeff_mod_count - 1; i++)
    {
        cout << coeff_modulus[i].bit_count() << " + ";
    }
    cout << coeff_modulus.back().bit_count();
    cout << ") bits" << endl;

    if (context_data.parms().scheme() == scheme_type::BFV)
    {
        cout << "|   plain_modulus: " << context_data.parms().plain_modulus().value() << endl;
    }

    cout << "\\" << endl;
}

// Helper function that prints a matrix (vector of vectors)
template <typename T>
inline void print_full_matrix(vector<vector<T>> matrix, int precision = 3)
{
    // save formatting for cout
    ios old_fmt(nullptr);
    old_fmt.copyfmt(cout);
    cout << fixed << setprecision(precision);
    int row_size = matrix.size();
    int col_size = matrix[0].size();
    for (unsigned int i = 0; i < row_size; i++)
    {
        cout << "[";
        for (unsigned int j = 0; j < col_size - 1; j++)
        {
            cout << matrix[i][j] << ", ";
        }
        cout << matrix[i][col_size - 1];
        cout << "]" << endl;
    }
    cout << endl;
    // restore old cout formatting
    cout.copyfmt(old_fmt);
}

// Helper function that prints parts of a matrix (only squared matrix)
template <typename T>
inline void print_partial_matrix(vector<vector<T>> matrix, int print_size = 3, int precision = 3)
{
    // save formatting for cout
    ios old_fmt(nullptr);
    old_fmt.copyfmt(cout);
    cout << fixed << setprecision(precision);

    int row_size = matrix.size();
    int col_size = matrix[0].size();

    // Boundary check
    if (row_size < 2 * print_size && col_size < 2 * print_size)
    {
        cerr << "Cannot print matrix with these dimensions: " << to_string(row_size) << "x" << to_string(col_size) << ". Increase the print size" << endl;
        return;
    }
    // print first 4 elements
    for (unsigned int row = 0; row < print_size; row++)
    {
        cout << "\t[";
        for (unsigned int col = 0; col < print_size; col++)
        {
            cout << matrix[row][col] << ", ";
        }
        cout << "..., ";
        for (unsigned int col = col_size - print_size; col < col_size - 1; col++)
        {
            cout << matrix[row][col] << ", ";
        }
        cout << matrix[row][col_size - 1];
        cout << "]" << endl;
    }
    cout << "\t..." << endl;

    for (unsigned int row = row_size - print_size; row < row_size; row++)
    {
        cout << "\t[";
        for (unsigned int col = 0; col < print_size; col++)
        {
            cout << matrix[row][col] << ", ";
        }
        cout << "..., ";
        for (unsigned int col = col_size - print_size; col < col_size - 1; col++)
        {
            cout << matrix[row][col] << ", ";
        }
        cout << matrix[row][col_size - 1];
        cout << "]" << endl;
    }

    cout << endl;
    // restore old cout formatting
    cout.copyfmt(old_fmt);
}

template <typename T>
inline void print_partial_vector(vector<T> vec, int size, int print_size = 3, int precision = 3)
{
    // save formatting for cout
    ios old_fmt(nullptr);
    old_fmt.copyfmt(cout);
    cout << fixed << setprecision(precision);

    int row_size = size;

    // Boundary check
    if (row_size < 2 * print_size)
    {
        cerr << "Cannot print vector with these dimensions: " << to_string(row_size) << ". Increase the print size" << endl;
        return;
    }

    cout << "\t[";
    for (unsigned int row = 0; row < print_size; row++)
    {
        cout << vec[row] << ", ";
    }
    cout << "..., ";

    for (unsigned int row = row_size - print_size; row < row_size - 1; row++)
    {
        cout << vec[row] << ", ";
    }
    cout << vec[row_size - 1] << "]\n";

    cout << endl;
    // restore old cout formatting
    cout.copyfmt(old_fmt);
}

// Gets a diagonal from a matrix U
template <typename T>
vector<T> get_diagonal(int position, vector<vector<T>> U)
{

    vector<T> diagonal(U.size());

    int k = 0;
    // U(0,l) , U(1,l+1), ... ,  U(n-l-1, n-1)
    for (int i = 0, j = position; (i < U.size() - position) && (j < U.size()); i++, j++)
    {
        diagonal[k] = U[i][j];
        k++;
    }
    for (int i = U.size() - position, j = 0; (i < U.size()) && (j < position); i++, j++)
    {
        diagonal[k] = U[i][j];
        k++;
    }

    return diagonal;
}

template <typename T>
vector<vector<T>> get_all_diagonals(vector<vector<T>> U)
{

    vector<vector<T>> diagonal_matrix(U.size());

    for (int i = 0; i < U.size(); i++)
    {
        diagonal_matrix[i] = get_diagonal(i, U);
    }

    return diagonal_matrix;
}

Ciphertext Linear_Transform_Cipher(Ciphertext ct, vector<Ciphertext> U_diagonals, GaloisKeys gal_keys, Evaluator &evaluator)
{
    // Fill ct with duplicate
    Ciphertext ct_rot;
    evaluator.rotate_vector(ct, -U_diagonals.size(), gal_keys, ct_rot);
    // cout << "U_diagonals.size() = " << U_diagonals.size() << endl;
    Ciphertext ct_new;
    evaluator.add(ct, ct_rot, ct_new);

    vector<Ciphertext> ct_result(U_diagonals.size());
    evaluator.multiply(ct_new, U_diagonals[0], ct_result[0]);

    for (int l = 1; l < U_diagonals.size(); l++)
    {
        Ciphertext temp_rot;
        evaluator.rotate_vector(ct_new, l, gal_keys, temp_rot);
        evaluator.multiply(temp_rot, U_diagonals[l], ct_result[l]);
    }
    Ciphertext ct_prime;
    evaluator.add_many(ct_result, ct_prime);

    return ct_prime;
}

// Linear transformation function between ciphertext matrix and plaintext vector
Ciphertext Linear_Transform_CipherMatrix_PlainVector(vector<Plaintext> pt_rotations, vector<Ciphertext> U_diagonals, GaloisKeys gal_keys, Evaluator &evaluator)
{
    vector<Ciphertext> ct_result(pt_rotations.size());

    for (int i = 0; i < pt_rotations.size(); i++)
    {
        evaluator.multiply_plain(U_diagonals[i], pt_rotations[i], ct_result[i]);
    }

    Ciphertext ct_prime;
    evaluator.add_many(ct_result, ct_prime);

    return ct_prime;
}

template <typename T>
vector<vector<double>> get_matrix_of_ones(int position, vector<vector<T>> U)
{
    vector<vector<double>> diagonal_of_ones(U.size(), vector<double>(U.size()));
    vector<T> U_diag = get_diagonal(position, U);

    int k = 0;
    for (int i = 0; i < U.size(); i++)
    {
        for (int j = 0; j < U.size(); j++)
        {
            if (U[i][j] == U_diag[k])
            {
                diagonal_of_ones[i][j] = 1;
            }
            else
            {
                diagonal_of_ones[i][j] = 0;
            }
        }
        k++;
    }

    return diagonal_of_ones;
}

// Encodes Ciphertext Matrix into a single vector (Row ordering of a matix)
Ciphertext C_Matrix_Encode(vector<Ciphertext> matrix, GaloisKeys gal_keys, Evaluator &evaluator)
{
    Ciphertext ct_result;
    int dimension = matrix.size();
    vector<Ciphertext> ct_rots(dimension);
    ct_rots[0] = matrix[0];

    for (int i = 1; i < dimension; i++)
    {
        evaluator.rotate_vector(matrix[i], (i * -dimension), gal_keys, ct_rots[i]);
    }

    evaluator.add_many(ct_rots, ct_result);

    return ct_result;
}

// Decodes Ciphertext Matrix into vector of Ciphertexts
vector<Ciphertext> C_Matrix_Decode(Ciphertext matrix, int dimension, double scale, GaloisKeys gal_keys, CKKSEncoder &ckks_encoder, Evaluator &evaluator)
{

    vector<Ciphertext> ct_result(dimension);
    for (int i = 0; i < dimension; i++)
    {
        // Create masks vector with 1s and 0s
        // Fill mask vector with 0s
        vector<double> mask_vec(pow(dimension, 2), 0);

        // Store 1s in mask vector at dimension offset. Offset = j + (i * dimension)
        for (int j = 0; j < dimension; j++)
        {
            mask_vec[j + (i * dimension)] = 1;
        }

        // Encode mask vector
        Plaintext mask_pt;
        ckks_encoder.encode(mask_vec, scale, mask_pt);

        // multiply matrix with mask
        Ciphertext ct_row;
        evaluator.multiply_plain(matrix, mask_pt, ct_row);

        // rotate row (not the first one)
        if (i != 0)
        {
            evaluator.rotate_vector_inplace(ct_row, i * dimension, gal_keys);
        }

        // store in result
        ct_result[i] = ct_row;
    }

    return ct_result;
}

template <typename T>
vector<double> pad_zero(int offset, vector<T> U_vec)
{

    vector<double> result_vec(pow(U_vec.size(), 2));
    // Fill before U_vec
    for (int i = 0; i < offset; i++)
    {
        result_vec[i] = 0;
    }
    // Fill U_vec
    for (int i = 0; i < U_vec.size(); i++)
    {
        result_vec[i + offset] = U_vec[i];
    }
    // Fill after U_vec
    for (int i = offset + U_vec.size(); i < result_vec.size(); i++)
    {
        result_vec[i] = 0;
    }
    return result_vec;
}

// U_transpose
template <typename T>
vector<vector<double>> get_U_transpose(vector<vector<T>> U)
{

    int dimension = U.size();
    int dimensionSq = pow(dimension, 2);
    vector<vector<double>> U_transpose(dimensionSq, vector<double>(dimensionSq));

    int tranposed_row = 0;

    for (int i = 0; i < dimension; i++)
    {
        // Get matrix of ones at position k
        vector<vector<double>> one_matrix = get_matrix_of_ones(i, U);
        print_full_matrix(one_matrix);

        // Loop over matrix of ones
        for (int offset = 0; offset < dimension; offset++)
        {
            vector<double> temp_fill = pad_zero(offset * dimension, one_matrix[0]);

            U_transpose[tranposed_row] = temp_fill;
            tranposed_row++;
        }
    }

    return U_transpose;
}

void compute_all_powers(const Ciphertext &ctx, int degree, Evaluator &evaluator, RelinKeys &relin_keys, vector<Ciphertext> &powers)
{

    powers.resize(degree + 1);
    powers[1] = ctx;

    vector<int> levels(degree + 1, 0);
    levels[1] = 0;
    levels[0] = 0;

    for (int i = 2; i <= degree; i++)
    {
        // compute x^i
        int minlevel = i;
        int cand = -1;
        for (int j = 1; j <= i / 2; j++)
        {
            int k = i - j;
            int newlevel = max(levels[j], levels[k]) + 1;
            if (newlevel < minlevel)
            {
                cand = j;
                minlevel = newlevel;
            }
        }

        levels[i] = minlevel;
        // use cand
        if (cand < 0)
            throw runtime_error("error");
        // cand <= i - cand by definition
        Ciphertext temp = powers[cand];
        evaluator.mod_switch_to_inplace(temp, powers[i - cand].parms_id());

        evaluator.multiply(temp, powers[i - cand], powers[i]);

        evaluator.relinearize_inplace(powers[i], relin_keys);

        evaluator.rescale_to_next_inplace(powers[i]);
    }

    return;
}

// Tree method for polynomial evaluation
void tree(int degree, double x)
{
    chrono::high_resolution_clock::time_point time_start, time_end;
    chrono::microseconds time_diff;

    EncryptionParameters parms(scheme_type::CKKS);

    int depth = ceil(log2(degree));

    vector<int> moduli(depth + 4, 40);
    moduli[0] = 50;
    moduli[moduli.size() - 1] = 59;

    size_t poly_modulus_degree = 16384;
    parms.set_poly_modulus_degree(poly_modulus_degree);
    parms.set_coeff_modulus(CoeffModulus::Create(
        poly_modulus_degree, moduli));

    double scale = pow(2.0, 40);

    auto context = SEALContext::Create(parms);

    KeyGenerator keygen(context);
    auto pk = keygen.public_key();
    auto sk = keygen.secret_key();
    auto relin_keys = keygen.relin_keys();
    Encryptor encryptor(context, pk);
    Decryptor decryptor(context, sk);

    Evaluator evaluator(context);
    CKKSEncoder ckks_encoder(context);

    print_parameters(context);
    cout << endl;

    Plaintext ptx;
    ckks_encoder.encode(x, scale, ptx);
    Ciphertext ctx;
    encryptor.encrypt(ptx, ctx);
    cout << "x = " << x << endl;

    vector<double> coeffs(degree + 1);
    vector<Plaintext> plain_coeffs(degree + 1);

    // Random Coefficients from 0-1
    cout << "Polynomial = ";
    int counter = 0;
    for (size_t i = 0; i < degree + 1; i++)
    {
        coeffs[i] = (double)rand() / RAND_MAX;
        ckks_encoder.encode(coeffs[i], scale, plain_coeffs[i]);
        cout << "x^" << counter << " * (" << coeffs[i] << ")"
             << ", ";
    }
    cout << endl;

    Plaintext plain_result;
    vector<double> result;

    /*
    decryptor.decrypt(ctx, plain_result);
    ckks_encoder.decode(plain_result, result);
    cout << "ctx  = " << result[0] << endl;
    */

    double expected_result = coeffs[degree];

    // Compute all powers
    vector<Ciphertext> powers(degree + 1);

    time_start = chrono::high_resolution_clock::now();

    compute_all_powers(ctx, degree, evaluator, relin_keys, powers);
    cout << "All powers computed " << endl;

    Ciphertext enc_result;
    // result = a[0]
    cout << "Encrypt first coeff...";
    encryptor.encrypt(plain_coeffs[0], enc_result);
    cout << "Done" << endl;

    /*
    for (int i = 1; i <= degree; i++){
        decryptor.decrypt(powers[i], plain_result);
        ckks_encoder.decode(plain_result, result);
        // cout << "power  = " << result[0] << endl;
    }
    */

    Ciphertext temp;

    // result += a[i]*x[i]
    for (int i = 1; i <= degree; i++)
    {

        // cout << i << "-th sum started" << endl;
        evaluator.mod_switch_to_inplace(plain_coeffs[i], powers[i].parms_id());
        evaluator.multiply_plain(powers[i], plain_coeffs[i], temp);

        evaluator.rescale_to_next_inplace(temp);
        evaluator.mod_switch_to_inplace(enc_result, temp.parms_id());

        // Manual Rescale
        enc_result.scale() = pow(2.0, 40);
        temp.scale() = pow(2.0, 40);

        evaluator.add_inplace(enc_result, temp);
        // cout << i << "-th sum done" << endl;
    }

    time_end = chrono::high_resolution_clock::now();
    time_diff = chrono::duration_cast<chrono::microseconds>(time_end - time_start);
    cout << "Evaluation Duration:\t" << time_diff.count() << " microseconds" << endl;

    // Compute Expected result
    for (int i = degree - 1; i >= 0; i--)
    {
        expected_result *= x;
        expected_result += coeffs[i];
    }

    decryptor.decrypt(enc_result, plain_result);
    ckks_encoder.decode(plain_result, result);

    cout << "Actual : " << result[0] << "\nExpected : " << expected_result << "\ndiff : " << abs(result[0] - expected_result) << endl;

    // TEST Garbage
}

template <typename T>
vector<T> rotate_vec(vector<T> input_vec, int num_rotations)
{
    if (num_rotations > input_vec.size())
    {
        cerr << "Invalid number of rotations" << endl;
        exit(EXIT_FAILURE);
    }

    vector<T> rotated_res(input_vec.size());
    for (int i = 0; i < input_vec.size(); i++)
    {
        rotated_res[i] = input_vec[(i + num_rotations) % (input_vec.size())];
    }

    return rotated_res;
}

// Sigmoid
float sigmoid(float z)
{
    return 1 / (1 + exp(-z));
}

// Degree 3 Polynomial approximation of sigmoid function
Ciphertext Tree_sigmoid_approx(Ciphertext ctx, int degree, double scale, vector<double> coeffs, CKKSEncoder &ckks_encoder, Evaluator &evaluator, Encryptor &encryptor, RelinKeys relin_keys, EncryptionParameters params)
{
    cout << "->" << __func__ << endl;

    auto context = SEALContext::Create(params);

    cout << "\nCTx Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(ctx.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(ctx.scale()) << endl;
    ios old_fmt(nullptr);
    old_fmt.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << ctx.scale() << endl;
    cout.copyfmt(old_fmt);
    cout << "\tSize:\t" << ctx.size() << endl;

    int depth = ceil(log2(degree));

    // vector<double> coeffs(degree + 1);
    vector<Plaintext> plain_coeffs(degree + 1);

    cout << "Polynomial = ";
    int counter = 0;
    for (size_t i = 0; i < degree + 1; i++)
    {
        // cout << "-> " << __LINE__ << endl;
        if (coeffs[i] == 0)
        {
            continue;
        }
        ckks_encoder.encode(coeffs[i], scale, plain_coeffs[i]);
        cout << "x^" << counter << " * (" << coeffs[i] << ")"
             << ", ";
        counter++;
    }
    cout << endl;

    Plaintext plain_result;
    vector<double> result;

    double expected_result = coeffs[degree];

    // Compute all powers
    vector<Ciphertext> powers(degree + 1);

    // cout << "-> " << __LINE__ << endl;

    compute_all_powers(ctx, degree, evaluator, relin_keys, powers);
    cout << "All powers computed " << endl;

    cout << "\nCTx Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(ctx.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(ctx.scale()) << endl;
    ios old_fmt1(nullptr);
    old_fmt1.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << ctx.scale() << endl;
    cout.copyfmt(old_fmt1);
    cout << "\tSize:\t" << ctx.size() << endl;

    Ciphertext enc_result;
    cout << "Encrypt first coeff...";
    encryptor.encrypt(plain_coeffs[0], enc_result);
    cout << "Done" << endl;

    cout << "\nenc_result Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(enc_result.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(enc_result.scale()) << endl;
    ios old_fmt2(nullptr);
    old_fmt2.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << enc_result.scale() << endl;
    cout.copyfmt(old_fmt2);
    cout << "\tSize:\t" << enc_result.size() << endl;

    Ciphertext temp;

    for (int i = 1; i <= degree; i++)
    {
        // cout << "-> " << __LINE__ << endl;

        evaluator.mod_switch_to_inplace(plain_coeffs[i], powers[i].parms_id());
        // cout << "-> " << __LINE__ << endl;

        evaluator.multiply_plain(powers[i], plain_coeffs[i], temp);
        // cout << "-> " << __LINE__ << endl;

        evaluator.rescale_to_next_inplace(temp);
        // cout << "-> " << __LINE__ << endl;

        evaluator.mod_switch_to_inplace(enc_result, temp.parms_id());
        // cout << "-> " << __LINE__ << endl;

        // Manual Rescale
        enc_result.scale() = pow(2.0, (int)log2(enc_result.scale()));
        temp.scale() = pow(2.0, (int)log2(enc_result.scale()));
        // cout << "-> " << __LINE__ << endl;

        evaluator.add_inplace(enc_result, temp);
    }
    // cout << "-> " << __LINE__ << endl;

    // // Compute Expected result
    // for (int i = degree - 1; i >= 0; i--)
    // {
    //     expected_result *= x;
    //     expected_result += coeffs[i];
    // }

    // decryptor.decrypt(enc_result, plain_result);
    // ckks_encoder.decode(plain_result, result);

    // cout << "Actual : " << result[0] << "\nExpected : " << expected_result << "\ndiff : " << abs(result[0] - expected_result) << endl;
    cout << "\nenc_result Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(enc_result.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(enc_result.scale()) << endl;
    ios old_fmt3(nullptr);
    old_fmt3.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << enc_result.scale() << endl;
    cout.copyfmt(old_fmt3);
    cout << "\tSize:\t" << enc_result.size() << endl;

    return enc_result;
}

// Ciphertext dot product
Ciphertext cipher_dot_product(Ciphertext ctA, Ciphertext ctB, int size, RelinKeys relin_keys, GaloisKeys gal_keys, Evaluator &evaluator)
{
    Ciphertext mult;

    // Component-wise multiplication
    evaluator.multiply(ctA, ctB, mult);
    evaluator.relinearize_inplace(mult, relin_keys);
    evaluator.rescale_to_next_inplace(mult);

    // Fill with duplicate
    Ciphertext zero_filled;
    evaluator.rotate_vector(mult, -size, gal_keys, zero_filled); // vector has zeros now
    Ciphertext dup;
    evaluator.add(mult, zero_filled, dup); // vector has duplicate now

    for (int i = 1; i < size; i++)
    {
        evaluator.rotate_vector_inplace(dup, 1, gal_keys);
        evaluator.add_inplace(mult, dup);
    }

    // Manual Rescale
    mult.scale() = pow(2, (int)log2(mult.scale()));

    return mult;
}

Ciphertext Horner_sigmoid_approx(Ciphertext ctx, int degree, vector<double> coeffs, CKKSEncoder &ckks_encoder, double scale, Evaluator &evaluator, Encryptor &encryptor, RelinKeys relin_keys, EncryptionParameters params)
{
    auto context = SEALContext::Create(params);

    cout << "->" << __func__ << endl;
    cout << "->" << __LINE__ << endl;

    cout << "\nCTx Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(ctx.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(ctx.scale()) << endl;
    ios old_fmt(nullptr);
    old_fmt.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << ctx.scale() << endl;
    cout.copyfmt(old_fmt);
    cout << "\tSize:\t" << ctx.size() << endl;

    vector<Plaintext> plain_coeffs(degree + 1);

    // Random Coefficients from 0-1
    cout << "Polynomial = ";
    int counter = 0;
    for (size_t i = 0; i < degree + 1; i++)
    {
        // coeffs[i] = (double)rand() / RAND_MAX;
        ckks_encoder.encode(coeffs[i], scale, plain_coeffs[i]);
        cout << "x^" << counter << " * (" << coeffs[i] << ")"
             << ", ";
        counter++;
    }
    cout << endl;
    // cout << "->" << __LINE__ << endl;

    Ciphertext temp;
    encryptor.encrypt(plain_coeffs[degree], temp);

    Plaintext plain_result;
    vector<double> result;
    // cout << "->" << __LINE__ << endl;

    for (int i = degree - 1; i >= 0; i--)
    {
        // cout << "->" << __LINE__ << endl;
        // cout << "\nCTx Info:\n";
        // cout << "\tLevel:\t" << context->get_context_data(ctx.parms_id())->chain_index() << endl;
        // cout << "\tScale:\t" << log2(ctx.scale()) << endl;
        // ios old_fmt1(nullptr);
        // old_fmt1.copyfmt(cout);
        // cout << fixed << setprecision(10);
        // cout << "\tExact Scale:\t" << ctx.scale() << endl;
        // cout.copyfmt(old_fmt1);
        // cout << "\tSize:\t" << ctx.size() << endl;

        // cout << "\ntemp Info:\n";
        // cout << "\tLevel:\t" << context->get_context_data(temp.parms_id())->chain_index() << endl;
        // cout << "\tScale:\t" << log2(temp.scale()) << endl;
        // ios old_fmt2(nullptr);
        // old_fmt2.copyfmt(cout);
        // cout << fixed << setprecision(10);
        // cout << "\tExact Scale:\t" << temp.scale() << endl;
        // cout.copyfmt(old_fmt2);
        // cout << "\tSize:\t" << temp.size() << endl;

        int ctx_level = context->get_context_data(ctx.parms_id())->chain_index();
        int temp_level = context->get_context_data(temp.parms_id())->chain_index();
        if (ctx_level > temp_level)
        {
            evaluator.mod_switch_to_inplace(ctx, temp.parms_id());
        }
        else if (ctx_level < temp_level)
        {
            evaluator.mod_switch_to_inplace(temp, ctx.parms_id());
        }
        evaluator.multiply_inplace(temp, ctx);
        // cout << "->" << __LINE__ << endl;

        evaluator.relinearize_inplace(temp, relin_keys);

        evaluator.rescale_to_next_inplace(temp);
        // cout << "->" << __LINE__ << endl;

        evaluator.mod_switch_to_inplace(plain_coeffs[i], temp.parms_id());

        // Manual rescale
        temp.scale() = pow(2.0, 40);
        // cout << "->" << __LINE__ << endl;

        evaluator.add_plain_inplace(temp, plain_coeffs[i]);
    }
    // cout << "->" << __LINE__ << endl;

    cout << "\ntemp Info:\n";
    cout << "\tLevel:\t" << context->get_context_data(temp.parms_id())->chain_index() << endl;
    cout << "\tScale:\t" << log2(temp.scale()) << endl;
    ios old_fmt1(nullptr);
    old_fmt1.copyfmt(cout);
    cout << fixed << setprecision(10);
    cout << "\tExact Scale:\t" << temp.scale() << endl;
    cout.copyfmt(old_fmt1);
    cout << "\tSize:\t" << temp.size() << endl;

    return temp;
}

// Predict Ciphertext Weights
Ciphertext predict_cipher_weights(vector<Ciphertext> features, Ciphertext weights, int num_weights, double scale, Evaluator &evaluator, CKKSEncoder &ckks_encoder, GaloisKeys gal_keys, RelinKeys relin_keys, Encryptor &encryptor, EncryptionParameters params)
{
    cout << "->" << __func__ << endl;
    cout << "->" << __LINE__ << endl;

    // Linear Transformation (loop over rows and dot product)
    int num_rows = features.size();
    vector<Ciphertext> results(num_rows);

    for (int i = 0; i < num_rows; i++)
    {
        // Dot Product
        results[i] = cipher_dot_product(features[i], weights, num_weights, relin_keys, gal_keys, evaluator);
        // Create mask
        vector<double> mask_vec(num_rows, 0);
        mask_vec[i] = 1;
        Plaintext mask_pt;
        ckks_encoder.encode(mask_vec, scale, mask_pt);
        // Bring down mask by 1 level since dot product consumed 1 level
        evaluator.mod_switch_to_next_inplace(mask_pt);
        // Multiply result with mask
        evaluator.multiply_plain_inplace(results[i], mask_pt);
        // MAYBE RELIN, RESCALE AND MANUAL RESCALE AFTER LOOP ????? ---------------------
        // // Relin
        // evaluator.relinearize_inplace(results[i], relin_keys);
        // // Rescale
        // evaluator.rescale_to_next_inplace(results[i]);
        // // Manual Rescale
        // results[i].scale() = pow(2, (int)log2(results[i].scale()));
    }
    // Add all results to ciphertext vec
    Ciphertext lintransf_vec;
    evaluator.add_many(results, lintransf_vec);
    cout << "->" << __LINE__ << endl;
    // MAYBE RELIN, RESCALE AND MANUAL RESCALE AFTER LOOP ????? ---------------------
    // Relin
    evaluator.relinearize_inplace(lintransf_vec, relin_keys);
    // Rescale
    evaluator.rescale_to_next_inplace(lintransf_vec);
    // Manual Rescale
    lintransf_vec.scale() = pow(2, (int)log2(lintransf_vec.scale()));
    cout << "->" << __LINE__ << endl;
    // Sigmoid over result
    vector<double> coeffs;
    if (DEGREE == 3)
    {
        coeffs = {0.5, 1.20069, 0.00001, -0.81562};
    }
    else if (DEGREE == 5)
    {
        coeffs = {0.5, 1.53048, 0.00001, -2.3533056, 0.00001, 1.3511295};
    }
    else if (DEGREE == 7)
    {
        coeffs = {0.5, 1.73496, 0.00001, -4.19407, 0.00001, 5.43402, 0.00001, -2.50739};
    }
    else
    {
        cerr << "Invalid DEGREE" << endl;
        exit(EXIT_SUCCESS);
    }

    Ciphertext predict_res = Horner_sigmoid_approx(lintransf_vec, coeffs.size() - 1, coeffs, ckks_encoder, scale, evaluator, encryptor, relin_keys, params);
    cout << "->" << __LINE__ << endl;
    return predict_res;
}

Ciphertext update_weights(vector<Ciphertext> features, vector<Ciphertext> features_T, Ciphertext labels, Ciphertext weights, float learning_rate, Evaluator &evaluator, CKKSEncoder &ckks_encoder, GaloisKeys gal_keys, RelinKeys relin_keys, Encryptor &encryptor, double scale, EncryptionParameters params)
{

    cout << "->" << __func__ << endl;
    cout << "->" << __LINE__ << endl;

    int num_observations = features.size();
    int num_weights = features_T.size();

    cout << "num obs = " << num_observations << endl;
    cout << "num weights = " << num_weights << endl;

    // cout << "->" << __func__ << endl;
    // cout << "->" << __LINE__ << endl;

    // Get predictions
    Ciphertext predictions = predict_cipher_weights(features, weights, num_weights, scale, evaluator, ckks_encoder, gal_keys, relin_keys, encryptor, params);

    // cout << "->" << __LINE__ << endl;

    // Calculate Predictions - Labels
    // Mod switch labels
    evaluator.mod_switch_to_inplace(labels, predictions.parms_id());
    Ciphertext pred_labels;
    evaluator.sub(predictions, labels, pred_labels);

    // cout << "->" << __LINE__ << endl;

    // Calculate Gradient vector (loop over rows and dot product)

    vector<Ciphertext> gradient_results(num_weights);
    for (int i = 0; i < num_weights; i++)
    {
        // Mod switch features T [i]
        evaluator.mod_switch_to_inplace(features_T[i], pred_labels.parms_id());
        gradient_results[i] = cipher_dot_product(features_T[i], pred_labels, num_observations, relin_keys, gal_keys, evaluator);

        // Create mask
        vector<double> mask_vec(num_weights, 0);
        mask_vec[i] = 1;
        Plaintext mask_pt;
        ckks_encoder.encode(mask_vec, scale, mask_pt);
        // Multiply result with mask
        evaluator.multiply_plain_inplace(gradient_results[i], mask_pt);
        // MAYBE RELIN, RESCALE AND MANUAL RESCALE AFTER LOOP ????? ---------------------
        // // Relin
        // evaluator.relinearize_inplace(gradient_results[i], relin_keys);
        // // Rescale
        // evaluator.rescale_to_next_inplace(gradient_results[i]);
        // // Manual rescale
        // gradient_results[i].scale() = pow(2, (int)log2(gradient_results[i].scale()));
    }
    // Add all gradient results to gradient
    Ciphertext gradient;
    evaluator.add_many(gradient_results, gradient);
    // MAYBE RELIN, RESCALE AND MANUAL RESCALE AFTER LOOP ????? ---------------------
    // Relin
    evaluator.relinearize_inplace(gradient, relin_keys);
    // Rescale
    evaluator.rescale_to_next_inplace(gradient);
    // Manual rescale
    gradient.scale() = pow(2, (int)log2(gradient.scale()));

    // Multiply by learning_rate/observations
    double N = learning_rate / num_observations;

    cout << "LR / num_obs = " << N << endl;

    Plaintext N_pt;
    ckks_encoder.encode(N, N_pt);
    // Mod Switch N_pt
    evaluator.mod_switch_to_inplace(N_pt, gradient.parms_id());
    evaluator.multiply_plain_inplace(gradient, N_pt);
    // cout << "->" << __LINE__ << endl;

    // Subtract from weights
    Ciphertext new_weights;
    evaluator.sub(gradient, weights, new_weights);
    evaluator.negate_inplace(new_weights);
    // cout << "->" << __LINE__ << endl;

    return new_weights;
}

Ciphertext train_cipher(vector<Ciphertext> features, vector<Ciphertext> features_T, Ciphertext labels, Ciphertext weights, float learning_rate, int iters, int observations, int num_weights, Evaluator &evaluator, CKKSEncoder &ckks_encoder, double scale, GaloisKeys gal_keys, RelinKeys relin_keys, Encryptor &encryptor, Decryptor &decryptor, EncryptionParameters params)
{
    cout << "->" << __func__ << endl;
    cout << "->" << __LINE__ << endl;

    // Copy weights to new_weights
    Ciphertext new_weights = weights;

    for (int i = 0; i < iters; i++)
    {

        // Get new weights
        new_weights = update_weights(features, features_T, labels, new_weights, learning_rate, evaluator, ckks_encoder, gal_keys, relin_keys, encryptor, scale, params);

        // Refresh weights
        Plaintext new_weights_pt;
        decryptor.decrypt(new_weights, new_weights_pt);
        vector<double> new_weights_decoded;
        ckks_encoder.decode(new_weights_pt, new_weights_decoded);

        // Log Progress
        if (i % 5 == 0)
        {
            cout << "\nIteration:\t" << i << endl;

            // Print weights
            cout << "Weights:\n\t[";
            for (int i = 0; i < num_weights; i++)
            {
                cout << new_weights_decoded[i] << ", ";
            }
            cout << "]" << endl;
        }

        encryptor.encrypt(new_weights_pt, new_weights);
    }

    return new_weights;
}

double sigmoid_approx_three(double x)
{
    cout << "->" << __func__ << endl;
    cout << "->" << __LINE__ << endl;

    double res;
    if (DEGREE == 3)
    {
        res = 0.5 + (1.20096 * (x / 8)) - (0.81562 * (pow((x / 8), 3)));
    }
    else if (DEGREE == 5)
    {
        res = 0.5 + (1.53048 * (x / 8)) - (2.3533056 * (pow((x / 8), 3))) + (1.3511295 * (pow((x / 8), 5)));
    }
    else if (DEGREE == 7)
    {
        res = 0.5 + (1.73496 * (x / 8)) - (4.19407 * (pow((x / 8), 3))) + (5.43402 * (pow((x / 8), 5))) - (2.50739 * (pow((x / 8), 3)));
    }
    else
    {
        cerr << "Invalid DEGREE" << endl;
        exit(EXIT_SUCCESS);
    }
    return res;
}

// CSV to string matrix converter
vector<vector<string>> CSVtoMatrix(string filename)
{
    vector<vector<string>> result_matrix;

    ifstream data(filename);
    string line;
    int line_count = 0;
    while (getline(data, line))
    {
        stringstream lineStream(line);
        string cell;
        vector<string> parsedRow;
        while (getline(lineStream, cell, ','))
        {
            parsedRow.push_back(cell);
        }
        // Skip first line since it has text instead of numbers
        if (line_count != 0)
        {
            result_matrix.push_back(parsedRow);
        }
        line_count++;
    }
    return result_matrix;
}

// String matrix to float matrix converter
vector<vector<double>> stringToDoubleMatrix(vector<vector<string>> matrix)
{
    vector<vector<double>> result(matrix.size(), vector<double>(matrix[0].size()));
    for (int i = 0; i < matrix.size(); i++)
    {
        for (int j = 0; j < matrix[0].size(); j++)
        {
            result[i][j] = ::atof(matrix[i][j].c_str());
            result[i][j] = static_cast<double>(result[i][j]);
        }
    }

    return result;
}

// Mean calculation
double getMean(vector<double> input_vec)
{
    float mean = 0;
    for (int i = 0; i < input_vec.size(); i++)
    {
        mean += input_vec[i];
    }
    mean /= input_vec.size();

    return mean;
}

// Standard Dev calculation
double getStandardDev(vector<double> input_vec, double mean)
{
    double variance = 0;
    for (int i = 0; i < input_vec.size(); i++)
    {
        variance += pow(input_vec[i] - mean, 2);
    }
    variance /= input_vec.size();

    double standard_dev = sqrt(variance);
    return standard_dev;
}

// Standard Scaler
vector<vector<double>> standard_scaler(vector<vector<double>> input_matrix)
{
    int rowSize = input_matrix.size();
    int colSize = input_matrix[0].size();
    vector<vector<double>> result_matrix(rowSize, vector<double>(colSize));

    // Optimization: Get Means and Standard Devs first then do the scaling
    // first pass: get means and standard devs
    vector<double> means_vec(colSize);
    vector<double> stdev_vec(colSize);
    for (int i = 0; i < colSize; i++)
    {
        vector<double> column(rowSize);
        for (int j = 0; j < rowSize; j++)
        {
            // cout << input_matrix[j][i] << ", ";
            column[j] = input_matrix[j][i];
            // cout << column[j] << ", ";
        }

        means_vec[i] = getMean(column);
        stdev_vec[i] = getStandardDev(column, means_vec[i]);
        // cout << "MEAN at i = " << i << ":\t" << means_vec[i] << endl;
        // cout << "STDV at i = " << i << ":\t" << stdev_vec[i] << endl;
    }

    // second pass: scale
    for (int i = 0; i < rowSize; i++)
    {
        for (int j = 0; j < colSize; j++)
        {
            result_matrix[i][j] = (input_matrix[i][j] - means_vec[j]) / stdev_vec[j];
            // cout << "RESULT at i = " << i << ":\t" << result_matrix[i][j] << endl;
        }
    }

    return result_matrix;
}

// Matrix Transpose
template <typename T>
vector<vector<T>> transpose_matrix(vector<vector<T>> input_matrix)
{

    int rowSize = input_matrix.size();
    int colSize = input_matrix[0].size();
    vector<vector<T>> transposed(colSize, vector<T>(rowSize));

    for (int i = 0; i < rowSize; i++)
    {
        for (int j = 0; j < colSize; j++)
        {
            transposed[j][i] = input_matrix[i][j];
        }
    }

    return transposed;
}

float RandomFloat(float a, float b)
{
    float random = ((float)rand()) / (float)RAND_MAX;
    float diff = b - a;
    float r = random * diff;
    return a + r;
}

int main()
{

    // Test evaluate sigmoid approx
    EncryptionParameters params(scheme_type::CKKS);

    // int depth = ceil(log2(DEGREE));

    // vector<int> moduli(depth + 4, 40);
    // moduli[0] = 50;
    // moduli[moduli.size() - 1] = 59;

    // size_t poly_modulus_degree = 16384;
    params.set_poly_modulus_degree(POLY_MOD_DEGREE);
    params.set_coeff_modulus(CoeffModulus::Create(POLY_MOD_DEGREE, {60, 40, 40, 40, 40, 40, 40, 40, 60}));

    double scale = pow(2.0, 40);

    auto context = SEALContext::Create(params);

    // Generate keys, encryptor, decryptor and evaluator
    KeyGenerator keygen(context);
    PublicKey pk = keygen.public_key();
    SecretKey sk = keygen.secret_key();
    GaloisKeys gal_keys = keygen.galois_keys();
    RelinKeys relin_keys = keygen.relin_keys();

    Encryptor encryptor(context, pk);
    Evaluator evaluator(context);
    Decryptor decryptor(context, sk);

    // Create CKKS encoder
    CKKSEncoder ckks_encoder(context);

    print_parameters(context);

    // -------------------------- TEST SIGMOID APPROXIMATION ---------------------------
    cout << "\n------------------- TEST SIGMOID APPROXIMATION -------------------\n"
         << endl;

    // Create data
    double x = 0.8;
    double x_eight = x / 8;
    Plaintext ptx;
    ckks_encoder.encode(x_eight, scale, ptx);
    Ciphertext ctx;
    encryptor.encrypt(ptx, ctx);

    // Create coeffs (Change with degree)
    vector<double> coeffs;
    if (DEGREE == 3)
    {
        coeffs = {0.5, 1.20069, 0.00001, -0.81562};
    }
    else if (DEGREE == 5)
    {
        coeffs = {0.5, 1.53048, 0.00001, -2.3533056, 0.00001, 1.3511295};
    }
    else if (DEGREE == 7)
    {
        coeffs = {0.5, 1.73496, 0.00001, -4.19407, 0.00001, 5.43402, 0.00001, -2.50739};
    }
    else
    {
        cerr << "Invalid DEGREE" << endl;
        exit(EXIT_SUCCESS);
    }

    // Multiply x by 1/8
    double eight = 1 / 8;
    Plaintext eight_pt;
    ckks_encoder.encode(eight, scale, eight_pt);
    // evaluator.multiply_plain_inplace(ctx, eight_pt);

    chrono::high_resolution_clock::time_point time_start, time_end;
    chrono::microseconds time_diff;
    time_start = chrono::high_resolution_clock::now();

    // Ciphertext ct_res_sigmoid = Tree_sigmoid_approx(ctx, DEGREE, scale, coeffs, ckks_encoder, evaluator, encryptor, relin_keys, params);
    Ciphertext ct_res_sigmoid = Horner_sigmoid_approx(ctx, DEGREE, coeffs, ckks_encoder, scale, evaluator, encryptor, relin_keys, params);
    time_end = chrono::high_resolution_clock::now();
    time_diff = chrono::duration_cast<chrono::microseconds>(time_end - time_start);
    cout << "Polynomial Evaluation Duration:\t" << time_diff.count() << " microseconds" << endl;

    // Decrypt and decode
    Plaintext pt_res_sigmoid;
    decryptor.decrypt(ct_res_sigmoid, pt_res_sigmoid);
    vector<double> res_sigmoid_vec;
    ckks_encoder.decode(pt_res_sigmoid, res_sigmoid_vec);

    // Get True expected result
    double true_expected_res = sigmoid(x_eight);

    // Get expected approximate result
    double expected_approx_res = sigmoid_approx_three(x);

    cout << "Actual Approximate Result =\t\t" << res_sigmoid_vec[0] << endl;
    cout << "Expected Approximate Result =\t\t" << expected_approx_res << endl;
    cout << "True Result =\t\t\t\t" << true_expected_res << endl;

    double difference = abs(res_sigmoid_vec[0] - true_expected_res);
    cout << "Approx. Error: Diff Actual and True =\t" << difference << endl;

    double horner_error = abs(res_sigmoid_vec[0] - expected_approx_res);
    cout << "CKKS Error: Diff Actual and Expected =\t" << horner_error << endl;

    // --------------------------- TEST LR -----------------------------------------
    cout << "\n--------------------------- TEST LR ---------------------------\n"
         << endl;

    // Read File
    string filename = "pulsar_stars_copy.csv";
    vector<vector<string>> s_matrix = CSVtoMatrix(filename);
    vector<vector<double>> f_matrix = stringToDoubleMatrix(s_matrix);

    // // Test print first 10 rows
    // cout << "First 10 rows of CSV file --------\n"
    //      << endl;
    // for (int i = 0; i < 10; i++)
    // {
    //     for (int j = 0; j < f_matrix[0].size(); j++)
    //     {
    //         cout << f_matrix[i][j] << ", ";
    //     }
    //     cout << endl;
    // }
    // cout << "...........\nLast 10 rows of CSV file ----------\n"
    //      << endl;
    // // Test print last 10 rows
    // for (int i = f_matrix.size() - 10; i < f_matrix.size(); i++)
    // {
    //     for (int j = 0; j < f_matrix[0].size(); j++)
    //     {
    //         cout << f_matrix[i][j] << ", ";
    //     }
    //     cout << endl;
    // }

    // Init features, labels and weights
    // Init features (rows of f_matrix , cols of f_matrix - 1)
    int rows = f_matrix.size();
    cout << "\nNumber of rows  = " << rows << endl;
    int cols = f_matrix[0].size() - 1;
    cout << "\nNumber of cols  = " << cols << endl;

    vector<vector<double>> features(rows, vector<double>(cols));
    // Init labels (rows of f_matrix)
    vector<double> labels(rows);
    // Init weight vector with zeros (cols of features)
    vector<double> weights(cols);

    // Fill the features matrix and labels vector
    for (int i = 0; i < rows; i++)
    {
        for (int j = 0; j < cols; j++)
        {
            features[i][j] = f_matrix[i][j];
        }
        labels[i] = f_matrix[i][cols];
    }

    // Fill the weights with random numbers (from 1 - 2)
    for (int i = 0; i < cols; i++)
    {
        weights[i] = RandomFloat(-2, 2);
    }

    // Test print the features and labels
    cout << "\nTesting features\n--------------\n"
         << endl;

    // Features Print test
    cout << "Features row size = " << features.size() << endl;
    cout << "Features col size = " << features[0].size() << endl;

    cout << "Labels row size = " << labels.size() << endl;
    cout << "Weights row size = " << weights.size() << endl;

    // for (int i = 0; i < 10; i++)
    // {
    //     for (int j = 0; j < features[0].size(); j++)
    //     {
    //         cout << features[i][j] << ", ";
    //     }
    //     cout << endl;
    // }

    // Standardize the features
    cout << "\nSTANDARDIZE TEST---------\n"
         << endl;

    vector<vector<double>> standard_features = standard_scaler(features);

    // // Test print first 10 rows
    // for (int i = 0; i < 10; i++)
    // {
    //     for (int j = 0; j < cols; j++)
    //     {
    //         cout << standard_features[i][j] << ", ";
    //     }
    //     cout << endl;
    // }
    // cout << "..........." << endl;
    // // Test print last 10 rows
    // for (int i = rows - 10; i < rows; i++)
    // {
    //     for (int j = 0; j < cols; j++)
    //     {
    //         cout << standard_features[i][j] << ", ";
    //     }
    //     cout << endl;
    // }

    // cout << "\nTesting labels\n--------------\n"
    //      << endl;

    // // Labels Print Test
    // for (int i = 0; i < 10; i++)
    // {
    //     cout << labels[i] << ", ";
    // }
    // cout << endl;

    // Print old weights
    cout << "\nOLD WEIGHTS\n------------------"
         << endl;
    for (int i = 0; i < weights.size(); i++)
    {
        cout << weights[i] << ", ";
    }
    cout << endl;

    // Get tranpose from client
    vector<vector<double>> features_T = transpose_matrix(features);
    // Get diagonals of transposed matrix
    // vector<vector<double>> features_T_diagonals = get_all_diagonals(features_T);

    // -------------- ENCODING ----------------
    // Encode features diagonals
    // vector<vector<double>> features_diagonals = get_all_diagonals(features);
    vector<Plaintext> features_pt(features.size());
    cout << "\nENCODING FEATURES ...";
    for (int i = 0; i < features.size(); i++)
    {
        ckks_encoder.encode(features[i], scale, features_pt[i]);
    }
    cout << "Done" << endl;

    vector<Plaintext> features_T_pt(features_T.size());
    cout << "\nENCODING TRANSPOSED FEATURES ...";
    for (int i = 0; i < features_T.size(); i++)
    {
        ckks_encoder.encode(features_T[i], scale, features_T_pt[i]);
    }
    cout << "Done" << endl;

    // Encode weights
    Plaintext weights_pt;
    cout << "\nENCODING WEIGHTS...";
    ckks_encoder.encode(weights, scale, weights_pt);
    cout << "Done" << endl;

    // Encode labels
    Plaintext labels_pt;
    cout << "\nENCODING LABELS...";
    ckks_encoder.encode(labels, scale, labels_pt);
    cout << "Done" << endl;

    // -------------- ENCRYPTING ----------------
    //Encrypt features diagonals
    vector<Ciphertext> features_ct(features.size());
    cout << "\nENCRYPTING FEATURES ...";
    for (int i = 0; i < features.size(); i++)
    {
        encryptor.encrypt(features_pt[i], features_ct[i]);
    }
    cout << "Done" << endl;

    vector<Ciphertext> features_T_ct(features_T.size());
    cout << "\nENCRYPTING TRANSPOSED FEATURES ...";
    for (int i = 0; i < features_T.size(); i++)
    {
        encryptor.encrypt(features_T_pt[i], features_T_ct[i]);
    }
    cout << "Done" << endl;

    // Encrypt weights
    Ciphertext weights_ct;
    cout << "\nENCRYPTING WEIGHTS...";
    encryptor.encrypt(weights_pt, weights_ct);
    cout << "Done" << endl;

    // Encrypt labels
    Ciphertext labels_ct;
    cout << "\nENCRYPTING LABELS...";
    encryptor.encrypt(labels_pt, labels_ct);
    cout << "Done" << endl;

    // --------------- TRAIN ---------------
    cout << "\nTraining--------------\n"
         << endl;

    // Get U_tranpose
    // vector<vector<double>> U_transpose = get_U_transpose(features);

    int observations = features.size();
    int num_weights = features[0].size();

    Ciphertext predictions;
    // predictions = predict_cipher_weights(features_diagonals_ct, weights_ct, num_weights, evaluator, ckks_encoder, gal_keys, relin_keys, encryptor);
    // predictions = predict_cipher_weights(features_ct, weights_ct, num_weights, scale, evaluator, ckks_encoder, gal_keys, relin_keys, encryptor, params);

    return 0;
}