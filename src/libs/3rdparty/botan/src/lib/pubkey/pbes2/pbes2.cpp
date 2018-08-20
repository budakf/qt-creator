/*
* PKCS #5 PBES2
* (C) 1999-2008,2014 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/pbes2.h>
#include <botan/cipher_mode.h>
#include <botan/pbkdf.h>
#include <botan/der_enc.h>
#include <botan/ber_dec.h>
#include <botan/parsing.h>
#include <botan/alg_id.h>
#include <botan/oids.h>
#include <botan/rng.h>

#if defined(BOTAN_HAS_SCRYPT)
   #include <botan/scrypt.h>
#endif

namespace Botan {

namespace {

SymmetricKey derive_key(const std::string& passphrase,
                        const AlgorithmIdentifier& kdf_algo,
                        size_t default_key_size)
   {
   if(kdf_algo.get_oid() == OIDS::lookup("PKCS5.PBKDF2"))
      {
      secure_vector<uint8_t> salt;
      size_t iterations = 0, key_length = 0;

      AlgorithmIdentifier prf_algo;
      BER_Decoder(kdf_algo.get_parameters())
         .start_cons(SEQUENCE)
         .decode(salt, OCTET_STRING)
         .decode(iterations)
         .decode_optional(key_length, INTEGER, UNIVERSAL)
         .decode_optional(prf_algo, SEQUENCE, CONSTRUCTED,
                          AlgorithmIdentifier("HMAC(SHA-160)",
                                              AlgorithmIdentifier::USE_NULL_PARAM))
         .end_cons();

      if(salt.size() < 8)
         throw Decoding_Error("PBE-PKCS5 v2.0: Encoded salt is too small");

      const std::string prf = OIDS::lookup(prf_algo.get_oid());

      std::unique_ptr<PBKDF> pbkdf(get_pbkdf("PBKDF2(" + prf + ")"));

      if(key_length == 0)
         key_length = default_key_size;

      return pbkdf->pbkdf_iterations(key_length, passphrase, salt.data(), salt.size(), iterations);
      }
#if defined(BOTAN_HAS_SCRYPT)
   else if(kdf_algo.get_oid() == OIDS::lookup("Scrypt"))
      {
      secure_vector<uint8_t> salt;
      size_t N = 0, r = 0, p = 0;
      size_t key_length = 0;

      AlgorithmIdentifier prf_algo;
      BER_Decoder(kdf_algo.get_parameters())
         .start_cons(SEQUENCE)
         .decode(salt, OCTET_STRING)
         .decode(N)
         .decode(r)
         .decode(p)
         .decode_optional(key_length, INTEGER, UNIVERSAL)
         .end_cons();

      if(key_length == 0)
         key_length = default_key_size;

      secure_vector<uint8_t> output(key_length);
      scrypt(output.data(), output.size(), passphrase,
             salt.data(), salt.size(), N, r, p);

      return SymmetricKey(output);
      }
#endif
   else
      throw Decoding_Error("PBE-PKCS5 v2.0: Unknown KDF algorithm " +
                           kdf_algo.get_oid().as_string());
   }

secure_vector<uint8_t> derive_key(const std::string& passphrase,
                                  const std::string& digest,
                                  RandomNumberGenerator& rng,
                                  size_t* msec_in_iterations_out,
                                  size_t iterations_if_msec_null,
                                  size_t key_length,
                                  AlgorithmIdentifier& kdf_algo)
   {
   const secure_vector<uint8_t> salt = rng.random_vec(12);

   if(digest == "Scrypt")
      {
#if defined(BOTAN_HAS_SCRYPT)

      Scrypt_Params params(32768, 8, 4);

      if(msec_in_iterations_out)
         params = Scrypt_Params(std::chrono::milliseconds(*msec_in_iterations_out));
      else
         params = Scrypt_Params(iterations_if_msec_null);

      secure_vector<uint8_t> key(key_length);
      scrypt(key.data(), key.size(), passphrase,
             salt.data(), salt.size(), params);

      std::vector<uint8_t> scrypt_params;
      DER_Encoder(scrypt_params)
         .start_cons(SEQUENCE)
            .encode(salt, OCTET_STRING)
            .encode(params.N())
            .encode(params.r())
            .encode(params.p())
            .encode(key_length)
         .end_cons();

      kdf_algo = AlgorithmIdentifier(OIDS::lookup("Scrypt"), scrypt_params);
      return key;
#else
      throw Not_Implemented("Scrypt is not available in this build");
#endif
      }
   else
      {
      const std::string prf = "HMAC(" + digest + ")";

      std::unique_ptr<PBKDF> pbkdf(get_pbkdf("PBKDF2(" + prf + ")"));

      size_t iterations = iterations_if_msec_null;

      secure_vector<uint8_t> key;

      if(msec_in_iterations_out)
         {
         std::chrono::milliseconds msec(*msec_in_iterations_out);
         key = pbkdf->derive_key(key_length, passphrase, salt.data(), salt.size(), msec, iterations).bits_of();
         *msec_in_iterations_out = iterations;
         }
      else
         {
         key = pbkdf->pbkdf_iterations(key_length, passphrase, salt.data(), salt.size(), iterations);
         }

      std::vector<uint8_t> pbkdf2_params;

      DER_Encoder(pbkdf2_params)
         .start_cons(SEQUENCE)
            .encode(salt, OCTET_STRING)
            .encode(iterations)
            .encode(key_length)
            .encode_if(prf != "HMAC(SHA-160)",
                       AlgorithmIdentifier(prf, AlgorithmIdentifier::USE_NULL_PARAM))
         .end_cons();

      kdf_algo = AlgorithmIdentifier("PKCS5.PBKDF2", pbkdf2_params);
      return key;
      }
   }

/*
* PKCS#5 v2.0 PBE Encryption
*/
std::pair<AlgorithmIdentifier, std::vector<uint8_t>>
pbes2_encrypt_shared(const secure_vector<uint8_t>& key_bits,
                     const std::string& passphrase,
                     size_t* msec_in_iterations_out,
                     size_t iterations_if_msec_null,
                     const std::string& cipher,
                     const std::string& prf,
                     RandomNumberGenerator& rng)
   {
   const std::vector<std::string> cipher_spec = split_on(cipher, '/');
   if(cipher_spec.size() != 2)
      throw Encoding_Error("PBE-PKCS5 v2.0: Invalid cipher spec " + cipher);

   if(cipher_spec[1] != "CBC" && cipher_spec[1] != "GCM")
      throw Encoding_Error("PBE-PKCS5 v2.0: Don't know param format for " + cipher);

   const OID cipher_oid = OIDS::lookup(cipher);
   if(cipher_oid.empty())
      throw Encoding_Error("PBE-PKCS5 v2.0: No OID assigned for " + cipher);

   std::unique_ptr<Cipher_Mode> enc = Cipher_Mode::create(cipher, ENCRYPTION);

   if(!enc)
      throw Decoding_Error("PBE-PKCS5 cannot encrypt no cipher " + cipher);

   const size_t key_length = enc->key_spec().maximum_keylength();

   const secure_vector<uint8_t> iv = rng.random_vec(enc->default_nonce_length());

   AlgorithmIdentifier kdf_algo;

   const secure_vector<uint8_t> derived_key =
      derive_key(passphrase, prf, rng,
                 msec_in_iterations_out, iterations_if_msec_null,
                 key_length, kdf_algo);

   enc->set_key(derived_key);
   enc->start(iv);
   secure_vector<uint8_t> ctext = key_bits;
   enc->finish(ctext);

   std::vector<uint8_t> pbes2_params;

   DER_Encoder(pbes2_params)
      .start_cons(SEQUENCE)
      .encode(kdf_algo)
      .encode(
         AlgorithmIdentifier(cipher,
            DER_Encoder().encode(iv, OCTET_STRING).get_contents_unlocked()
            )
         )
      .end_cons();

   AlgorithmIdentifier id(OIDS::lookup("PBE-PKCS5v20"), pbes2_params);

   return std::make_pair(id, unlock(ctext));
   }


}

std::pair<AlgorithmIdentifier, std::vector<uint8_t>>
pbes2_encrypt(const secure_vector<uint8_t>& key_bits,
              const std::string& passphrase,
              std::chrono::milliseconds msec,
              const std::string& cipher,
              const std::string& digest,
              RandomNumberGenerator& rng)
   {
   size_t msec_in_iterations_out = static_cast<size_t>(msec.count());
   return pbes2_encrypt_shared(key_bits, passphrase, &msec_in_iterations_out, 0, cipher, digest, rng);
   // return value msec_in_iterations_out discarded
   }

std::pair<AlgorithmIdentifier, std::vector<uint8_t>>
pbes2_encrypt_msec(const secure_vector<uint8_t>& key_bits,
                   const std::string& passphrase,
                   std::chrono::milliseconds msec,
                   size_t* out_iterations_if_nonnull,
                   const std::string& cipher,
                   const std::string& digest,
                   RandomNumberGenerator& rng)
   {
   size_t msec_in_iterations_out = static_cast<size_t>(msec.count());

   auto ret = pbes2_encrypt_shared(key_bits, passphrase, &msec_in_iterations_out, 0, cipher, digest, rng);

   if(out_iterations_if_nonnull)
      *out_iterations_if_nonnull = msec_in_iterations_out;

   return ret;
   }

std::pair<AlgorithmIdentifier, std::vector<uint8_t>>
pbes2_encrypt_iter(const secure_vector<uint8_t>& key_bits,
                   const std::string& passphrase,
                   size_t pbkdf_iter,
                   const std::string& cipher,
                   const std::string& digest,
                   RandomNumberGenerator& rng)
   {
   return pbes2_encrypt_shared(key_bits, passphrase, nullptr, pbkdf_iter, cipher, digest, rng);
   }

secure_vector<uint8_t>
pbes2_decrypt(const secure_vector<uint8_t>& key_bits,
              const std::string& passphrase,
              const std::vector<uint8_t>& params)
   {
   AlgorithmIdentifier kdf_algo, enc_algo;

   BER_Decoder(params)
      .start_cons(SEQUENCE)
         .decode(kdf_algo)
         .decode(enc_algo)
      .end_cons();

   const std::string cipher = OIDS::lookup(enc_algo.get_oid());
   const std::vector<std::string> cipher_spec = split_on(cipher, '/');
   if(cipher_spec.size() != 2)
      throw Decoding_Error("PBE-PKCS5 v2.0: Invalid cipher spec " + cipher);
   if(cipher_spec[1] != "CBC" && cipher_spec[1] != "GCM")
      throw Decoding_Error("PBE-PKCS5 v2.0: Don't know param format for " + cipher);

   secure_vector<uint8_t> iv;
   BER_Decoder(enc_algo.get_parameters()).decode(iv, OCTET_STRING).verify_end();

   std::unique_ptr<Cipher_Mode> dec = Cipher_Mode::create(cipher, DECRYPTION);
   if(!dec)
      throw Decoding_Error("PBE-PKCS5 cannot decrypt no cipher " + cipher);

   dec->set_key(derive_key(passphrase, kdf_algo, dec->key_spec().maximum_keylength()));

   dec->start(iv);

   secure_vector<uint8_t> buf = key_bits;
   dec->finish(buf);

   return buf;
   }

}