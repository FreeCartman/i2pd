#include <zlib.h> // for crc32
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include "Base.h"
#include "Crypto.h"
#include "Log.h"
#include "Timestamp.h"
#include "I2PEndian.h"
#include "Ed25519.h"
#include "Blinding.h"

namespace i2p
{
namespace data
{
	BlindedPublicKey::BlindedPublicKey (std::shared_ptr<const IdentityEx> identity, SigningKeyType blindedKeyType):
		m_BlindedSigType (blindedKeyType)
	{
		if (!identity) return;
		auto len = identity->GetSigningPublicKeyLen ();
		m_PublicKey.resize (len);
		memcpy (m_PublicKey.data (), identity->GetSigningPublicKeyBuffer (), len);
		m_SigType = identity->GetSigningKeyType ();
	}

	BlindedPublicKey::BlindedPublicKey (const std::string& b33)
	{
		uint8_t addr[40]; // TODO: define length from b33
		size_t l = i2p::data::Base32ToByteStream (b33.c_str (), b33.length (), addr, 40);
		uint32_t checksum = crc32 (0, addr + 3, l - 3); 
		// checksum is Little Endian
		addr[0] ^= checksum; addr[1] ^= (checksum >> 8); addr[2] ^= (checksum >> 16);  
		uint8_t flag = addr[0];
		size_t offset = 1;	
		if (flag & 0x01) // two bytes signatures
		{
			m_SigType = bufbe16toh (addr + offset); offset += 2;
			m_BlindedSigType = bufbe16toh (addr + offset); offset += 2;
		}
		else // one byte sig
		{
			m_SigType = addr[offset]; offset++;
			m_BlindedSigType = addr[offset]; offset++;
		}
		std::unique_ptr<i2p::crypto::Verifier> blindedVerifier (i2p::data::IdentityEx::CreateVerifier (m_SigType));
		if (blindedVerifier)
		{
			auto len = blindedVerifier->GetPublicKeyLen ();
			if (offset + len <= l)
			{
				m_PublicKey.resize (len);
				memcpy (m_PublicKey.data (), addr + offset, len);
			}
			else
				LogPrint (eLogError, "Blinding: public key in b33 address is too short for signature type ", (int)m_SigType);	
		}
		else
			LogPrint (eLogError, "Blinding: unknown signature type ", (int)m_SigType, " in b33");
	}

	std::string BlindedPublicKey::ToB33 () const
	{
		if (m_PublicKey.size () > 32) return ""; // assume 25519
		uint8_t addr[35]; char str[60]; // TODO: define actual length
		addr[0] = 0; // flags
		addr[1] = m_SigType; // sig type
		addr[2] = m_BlindedSigType; // blinded sig type
		memcpy (addr + 3, m_PublicKey.data (), m_PublicKey.size ());
		uint32_t checksum = crc32 (0, addr + 3, m_PublicKey.size ()); 
		// checksum is Little Endian
		addr[0] ^= checksum; addr[1] ^= (checksum >> 8); addr[2] ^= (checksum >> 16); 
		auto l = ByteStreamToBase32 (addr, m_PublicKey.size () + 3, str, 60);
		return std::string (str, str + l);
	}

	void BlindedPublicKey::GetCredential (uint8_t * credential) const
	{
		// A = destination's signing public key 
		// stA = signature type of A, 2 bytes big endian
		uint16_t stA = htobe16 (GetSigType ());
		// stA1 = signature type of blinded A, 2 bytes big endian
		uint16_t stA1 = htobe16 (GetBlindedSigType ());	
		// credential = H("credential", A || stA || stA1)
		H ("credential", { {GetPublicKey (), GetPublicKeyLen ()}, {(const uint8_t *)&stA, 2}, {(const uint8_t *)&stA1, 2} }, credential);
	}

	void BlindedPublicKey::GetSubcredential (const uint8_t * blinded, size_t len, uint8_t * subcredential) const
	{
		uint8_t credential[32];
		GetCredential (credential);
		// subcredential = H("subcredential", credential || blindedPublicKey)
		H ("subcredential", { {credential, 32}, {blinded, len} }, subcredential);
	}

	void BlindedPublicKey::GenerateAlpha (const char * date, uint8_t * seed) const
	{
		uint16_t stA = htobe16 (GetSigType ()), stA1 = htobe16 (GetBlindedSigType ());
		uint8_t salt[32];
		//seed = HKDF(H("I2PGenerateAlpha", keydata), datestring || secret, "i2pblinding1", 64)	
		H ("I2PGenerateAlpha", { {GetPublicKey (), GetPublicKeyLen ()}, {(const uint8_t *)&stA, 2}, {(const uint8_t *)&stA1, 2} }, salt);
		i2p::crypto::HKDF (salt, (const uint8_t *)date, 8, "i2pblinding1", seed);
	}

	void BlindedPublicKey::GetBlindedKey (const char * date, uint8_t * blindedKey) const
	{
		uint8_t seed[64];	
		GenerateAlpha (date, seed);	
		i2p::crypto::GetEd25519 ()->BlindPublicKey (GetPublicKey (), seed, blindedKey);
	}

	void BlindedPublicKey::BlindPrivateKey (const uint8_t * priv, const char * date, uint8_t * blindedPriv, uint8_t * blindedPub) const
	{
		uint8_t seed[64];	
		GenerateAlpha (date, seed);	
		i2p::crypto::GetEd25519 ()->BlindPrivateKey (priv, seed, blindedPriv, blindedPub);
	}

	void BlindedPublicKey::H (const std::string& p, const std::vector<std::pair<const uint8_t *, size_t> >& bufs, uint8_t * hash) const 
	{
		SHA256_CTX ctx;
		SHA256_Init (&ctx);
		SHA256_Update (&ctx, p.c_str (), p.length ());
		for (const auto& it: bufs)	
			SHA256_Update (&ctx, it.first, it.second);
		SHA256_Final (hash, &ctx);
	}

	i2p::data::IdentHash BlindedPublicKey::GetStoreHash (const char * date) const
	{
		i2p::data::IdentHash hash;
		if (m_BlindedSigType == i2p::data::SIGNING_KEY_TYPE_REDDSA_SHA512_ED25519 ||
			m_BlindedSigType == SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519)
		{
			uint8_t blinded[32];
			if (date)
				GetBlindedKey (date, blinded);
			else
			{
				char currentDate[9];
				i2p::util::GetCurrentDate (currentDate);
				GetBlindedKey (currentDate, blinded);	
			}	
			auto stA1 = htobe16 (m_BlindedSigType);
			SHA256_CTX ctx;
			SHA256_Init (&ctx);
			SHA256_Update (&ctx, (const uint8_t *)&stA1, 2);
			SHA256_Update (&ctx, blinded, 32);
			SHA256_Final ((uint8_t *)hash, &ctx);
		}
		else
			LogPrint (eLogError, "Blinding: blinded key type ", (int)m_BlindedSigType, " is not supported");			
		return hash;
	}

}
}

