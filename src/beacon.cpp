#include "beacon.h"
#include "util.h"
#include "uint256.h"
#include "key.h"
#include "main.h"
#include "appcache.h"
#include "contract/contract.h"

std::string RetrieveBeaconValueWithMaxAge(const std::string& cpid, int64_t iMaxSeconds);
int64_t GetRSAWeightByCPIDWithRA(std::string cpid);

std::string ExtractXML(std::string XMLdata, std::string key, std::string key_end);

namespace
{
    std::string GetNetSuffix()
    {
        return fTestNet ? "testnet" : "";
    }
}

bool GenerateBeaconKeys(const std::string &cpid, std::string &sOutPubKey, std::string &sOutPrivKey)
{
    // First Check the Index - if it already exists, use it
    sOutPrivKey = GetArgument("privatekey" + cpid + GetNetSuffix(), "");
    sOutPubKey  = GetArgument("publickey" + cpid + GetNetSuffix(), "");

    // If current keypair is not empty, but is invalid, allow the new keys to be stored, otherwise return 1: (10-25-2016)
    if (!sOutPrivKey.empty() && !sOutPubKey.empty())
    {
        uint256 hashBlock = GetRandHash();
        std::string sSignature;
        std::string sError;
        bool fResult;
        fResult = SignBlockWithCPID(cpid, hashBlock.GetHex(), sSignature, sError, true);

        if (!fResult)
            LogPrintf("GenerateBeaconKeys::Failed to sign block with cpid with existing keys; generating new key pair -> %s", sError);

        else
        {
            fResult = VerifyCPIDSignature(cpid, hashBlock.GetHex(), sSignature);

            if (fResult)
            {
                LogPrintf("GenerateBeaconKeys::Current keypair is valid.");
                return true;
            }

            else
                LogPrintf("GenerateBeaconKeys::Signing block with CPID was successful; However Verifying CPID Sign was not; Key pair is not valid, generating new key pair");
        }
    }

    // Generate the Keypair
    CKey key;
    key.MakeNewKey(false);
    CPrivKey vchPrivKey = key.GetPrivKey();
    sOutPrivKey = HexStr<CPrivKey::iterator>(vchPrivKey.begin(), vchPrivKey.end());
    sOutPubKey = HexStr(key.GetPubKey().Raw());

    return true;
}

bool StoreBeaconKeys(
        const std::string &cpid,
        const std::string &pubKey,
        const std::string &privKey)
{
    if (   !WriteKey("publickey" + cpid + GetNetSuffix(), pubKey)
        || !WriteKey("privatekey" + cpid + GetNetSuffix(), privKey)
       )
        return false;

    else
        return true;
}

std::string GetStoredBeaconPrivateKey(const std::string& cpid)
{
    return GetArgument("privatekey" + cpid + GetNetSuffix(), "");
}

std::string GetStoredBeaconPublicKey(const std::string& cpid)
{
    return GetArgument("publickey" + cpid + GetNetSuffix(), "");
}

void ActivateBeaconKeys(
        const std::string &cpid,
        const std::string &pubKey,
        const std::string &privKey)
{
    SetArgument("publickey" + cpid + GetNetSuffix(), pubKey);
    SetArgument("privatekey" + cpid + GetNetSuffix(), privKey);
}

void GetBeaconElements(const std::string& sBeacon, std::string& out_cpid, std::string& out_address, std::string& out_publickey)
{
    if (sBeacon.empty()) return;
    std::string sContract = DecodeBase64(sBeacon);
    std::vector<std::string> vContract = split(sContract.c_str(),";");
    if (vContract.size() < 4) return;
    out_cpid = vContract[0];
    out_address = vContract[2];
    out_publickey = vContract[3];
}

std::string GetBeaconPublicKey(const std::string& cpid, bool bAdvertisingBeacon)
{
    //3-26-2017 - Ensure beacon public key is within 6 months of network age (If advertising, let it be returned as missing after 5 months, to ensure the public key is renewed seamlessly).
    int iMonths = bAdvertisingBeacon ? 5 : 6;
    int64_t iMaxSeconds = 60 * 24 * 30 * iMonths * 60;
    std::string sBeacon = RetrieveBeaconValueWithMaxAge(cpid, iMaxSeconds);
    if (sBeacon.empty()) return "";
    // Beacon data structure: CPID,hashRand,Address,beacon public key: base64 encoded
    std::string sContract = DecodeBase64(sBeacon);
    std::vector<std::string> vContract = split(sContract.c_str(),";");
    if (vContract.size() < 4) return "";
    std::string sBeaconPublicKey = vContract[3];
    return sBeaconPublicKey;
}

int64_t BeaconTimeStamp(const std::string& cpid, bool bZeroOutAfterPOR)
{
    const AppCacheEntry& entry =  ReadCache("beacon", cpid);
    std::string sBeacon = entry.value;
    int64_t iLocktime = entry.timestamp;
    int64_t iRSAWeight = GetRSAWeightByCPIDWithRA(cpid);
    if (fDebug10)
        LogPrintf("Beacon %s, Weight %" PRId64 ", Locktime %" PRId64, sBeacon, iRSAWeight, iLocktime);
    if (bZeroOutAfterPOR && iRSAWeight==0)
        iLocktime = 0;
    return iLocktime;

}

bool HasActiveBeacon(const std::string& cpid)
{
    return GetBeaconPublicKey(cpid, false).empty() == false;
}

std::string RetrieveBeaconValueWithMaxAge(const std::string& cpid, int64_t iMaxSeconds)
{
    const AppCacheEntry& entry = ReadCache("beacon", cpid);

    // Compare the age of the beacon to the age of the current block. If we have
    // no current block we assume that the beacon is valid.
    int64_t iAge = pindexBest != NULL
          ? pindexBest->nTime - entry.timestamp
          : 0;

    return (iAge > iMaxSeconds)
          ? ""
          : entry.value;
}

bool VerifyBeaconContractTx(const CTransaction& tx)
{
    // Check if tx contains beacon advertisement and evaluate for certain conditions
    std::string chkMessageType = ExtractXML(tx.hashBoinc, "<MT>", "</MT>");
    std::string chkMessageAction = ExtractXML(tx.hashBoinc, "<MA>", "</MA>");

    if (chkMessageType != "beacon")
        return true; // Not beacon contract

    if (chkMessageAction != "A")
        return true; // Not an add contract for beacon

    std::string chkMessageContract = ExtractXML(tx.hashBoinc, "<MV>", "</MV>");
    std::string chkMessageContractCPID = ExtractXML(tx.hashBoinc, "<MK>", "</MK>");
    // Here we GetBeaconElements for the contract in the tx
    std::string tx_out_cpid;
    std::string tx_out_address;
    std::string tx_out_publickey;

    GetBeaconElements(chkMessageContract, tx_out_cpid, tx_out_address, tx_out_publickey);

    if (tx_out_cpid.empty() || tx_out_address.empty() || tx_out_publickey.empty() || chkMessageContractCPID.empty())
        return false; // Incomplete contract

    const AppCacheEntry& beaconEntry = ReadCache("beacon", chkMessageContractCPID);
    if (beaconEntry.value.empty())
    {
        if (fDebug10)
            LogPrintf("VBCTX : No Previous beacon found for CPID %s", chkMessageContractCPID);

        return true; // No previous beacon in cache
    }

    int64_t chkiAge = pindexBest != NULL
        ? tx.nLockTime - beaconEntry.timestamp
        : 0;
    int64_t chkSecondsBase = 60 * 24 * 30 * 60;

    // Conditions
    // Condition a) if beacon is younger then 5 months deny tx
    if (chkiAge <= chkSecondsBase * 5 && chkiAge >= 1)
    {
        if (fDebug10)
            LogPrintf("VBCTX : Beacon age violation. Beacon Age %" PRId64 " < Required Age %" PRId64, chkiAge, (chkSecondsBase * 5));

        return false;
    }

    // Condition b) if beacon is younger then 6 months but older then 5 months verify using the same keypair; if not deny tx
    if (chkiAge >= chkSecondsBase * 5 && chkiAge <= chkSecondsBase * 6)
    {
        std::string chk_out_cpid;
        std::string chk_out_address;
        std::string chk_out_publickey;

        // Here we GetBeaconElements for the contract in the current beacon in chain
        GetBeaconElements(beaconEntry.value, chk_out_cpid, chk_out_address, chk_out_publickey);

        if (tx_out_publickey != chk_out_publickey)
        {
            if (fDebug10)
                LogPrintf("VBCTX : Beacon tx publickey != publickey in chain. %s != %s", tx_out_publickey, chk_out_publickey);

            return false;
        }
    }

    // Passed checks
    return true;
}
