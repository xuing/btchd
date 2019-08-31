// Copyright (c) 2017-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/tx_verify.h>

#include <chain.h>
#include <coins.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <utilmoneystr.h>
#include <validation.h>

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    if (tx.nLockTime == 0)
        return true;
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    for (const auto& txin : tx.vin) {
        if (!(txin.nSequence == CTxIn::SEQUENCE_FINAL))
            return false;
    }
    return true;
}

std::pair<int, int64_t> CalculateSequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    assert(prevHeights->size() == tx.vin.size());

    // Will be set to the equivalent height- and time-based nLockTime
    // values that would be necessary to satisfy all relative lock-
    // time constraints given our view of block chain history.
    // The semantics of nLockTime are the last invalid height/time, so
    // use -1 to have the effect of any height or time being valid.
    int nMinHeight = -1;
    int64_t nMinTime = -1;

    // tx.nVersion is signed integer so requires cast to unsigned otherwise
    // we would be doing a signed comparison and half the range of nVersion
    // wouldn't support BIP 68.
    bool fEnforceBIP68 = static_cast<uint32_t>(tx.nVersion) >= 2
                      && flags & LOCKTIME_VERIFY_SEQUENCE;

    // Do not enforce sequence numbers as a relative lock time
    // unless we have been instructed to
    if (!fEnforceBIP68) {
        return std::make_pair(nMinHeight, nMinTime);
    }

    for (size_t txinIndex = 0; txinIndex < tx.vin.size(); txinIndex++) {
        const CTxIn& txin = tx.vin[txinIndex];

        // Sequence numbers with the most significant bit set are not
        // treated as relative lock-times, nor are they given any
        // consensus-enforced meaning at this point.
        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            // The height of this input is not relevant for sequence locks
            (*prevHeights)[txinIndex] = 0;
            continue;
        }

        int nCoinHeight = (*prevHeights)[txinIndex];

        if (txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG) {
            int64_t nCoinTime = block.GetAncestor(std::max(nCoinHeight-1, 0))->GetMedianTimePast();
            // NOTE: Subtract 1 to maintain nLockTime semantics
            // BIP 68 relative lock times have the semantics of calculating
            // the first block or time at which the transaction would be
            // valid. When calculating the effective block time or height
            // for the entire transaction, we switch to using the
            // semantics of nLockTime which is the last invalid block
            // time or height.  Thus we subtract 1 from the calculated
            // time or height.

            // Time-based relative lock-times are measured from the
            // smallest allowed timestamp of the block containing the
            // txout being spent, which is the median time past of the
            // block prior.
            nMinTime = std::max(nMinTime, nCoinTime + (int64_t)((txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) << CTxIn::SEQUENCE_LOCKTIME_GRANULARITY) - 1);
        } else {
            nMinHeight = std::max(nMinHeight, nCoinHeight + (int)(txin.nSequence & CTxIn::SEQUENCE_LOCKTIME_MASK) - 1);
        }
    }

    return std::make_pair(nMinHeight, nMinTime);
}

bool EvaluateSequenceLocks(const CBlockIndex& block, std::pair<int, int64_t> lockPair)
{
    assert(block.pprev);
    int64_t nBlockTime = block.pprev->GetMedianTimePast();
    if (lockPair.first >= block.nHeight || lockPair.second >= nBlockTime)
        return false;

    return true;
}

bool SequenceLocks(const CTransaction &tx, int flags, std::vector<int>* prevHeights, const CBlockIndex& block)
{
    return EvaluateSequenceLocks(block, CalculateSequenceLocks(tx, flags, prevHeights, block));
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    for (const auto& txin : tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    for (const auto& txout : tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const CCoinsViewCache& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int64_t GetTransactionSigOpCost(const CTransaction& tx, const CCoinsViewCache& inputs, int flags)
{
    int64_t nSigOps = GetLegacySigOpCount(tx) * WITNESS_SCALE_FACTOR;

    if (tx.IsCoinBase())
        return nSigOps;

    if (flags & SCRIPT_VERIFY_P2SH) {
        nSigOps += GetP2SHSigOpCount(tx, inputs) * WITNESS_SCALE_FACTOR;
    }

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
        assert(!coin.IsSpent());
        const CTxOut &prevout = coin.out;
        nSigOps += CountWitnessSigOps(tx.vin[i].scriptSig, prevout.scriptPubKey, &tx.vin[i].scriptWitness, flags);
    }
    return nSigOps;
}

bool CheckTransaction(const CTransaction& tx, CValidationState &state, bool fCheckDuplicateInputs)
{
    // Basic checks that don't depend on any context
    if (tx.vin.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vin-empty");
    if (tx.vout.empty())
        return state.DoS(10, false, REJECT_INVALID, "bad-txns-vout-empty");
    // Size limits (this doesn't take the witness into account, as that hasn't been checked for malleability)
    if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) * WITNESS_SCALE_FACTOR > MAX_BLOCK_WEIGHT)
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-oversize");

    // Check for negative or overflow output values
    CAmount nValueOut = 0;
    for (const auto& txout : tx.vout)
    {
        if (txout.nValue < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-negative");
        if (txout.nValue > MAX_MONEY)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-vout-toolarge");
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-txouttotal-toolarge");
    }

    // Check for duplicate inputs - note that this check is slow so we skip it in CheckBlock
    if (fCheckDuplicateInputs) {
        std::set<COutPoint> vInOutPoints;
        for (const auto& txin : tx.vin)
        {
            if (!vInOutPoints.insert(txin.prevout).second)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-duplicate");
        }
    }

    if (tx.IsCoinBase())
    {
        if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
    }
    else
    {
        for (const auto& txin : tx.vin)
            if (txin.prevout.IsNull())
                return state.DoS(10, false, REJECT_INVALID, "bad-txns-prevout-null");
    }

    return true;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, const CCoinsViewCache& prevInputs,
    int nSpendHeight, CAmount& txfee, const CAccountID& generatorAccountID, bool fStrictCheckLimit, const Consensus::Params& params)
{
    // are the actual inputs available?
    if (!inputs.HaveInputs(tx)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-missingorspent", false,
                         strprintf("%s: inputs missing/spent", __func__));
    }

    CAmount nValueIn = 0;
    for (unsigned int i = 0; i < tx.vin.size(); ++i) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase() && nSpendHeight - coin.nHeight < COINBASE_MATURITY) {
            return state.Invalid(false,
                REJECT_INVALID, "bad-txns-premature-spend-of-coinbase",
                strprintf("tried to spend coinbase at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn += coin.out.nValue;
        if (!MoneyRange(coin.out.nValue) || !MoneyRange(nValueIn)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");
        }

        // Check special coin spend
        if (coin.extraData && (!tx.IsUniform() || tx.vin.size() != 1 || tx.vout.size() != 1))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-spend-special-coin");
    }

    const CAmount value_out = tx.GetValueOut();
    if (nValueIn < value_out) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
            strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(value_out)));
    }

    // Tally transaction fees
    CAmount txfee_aux = nValueIn - value_out;
    if (!MoneyRange(txfee_aux)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    }

    // Check uniform transaction. Inputs[i] == Outputs[j]
    if (tx.IsUniform() && nSpendHeight >= params.BHDIP006Height) {
        const CScript& scriptPubKey = inputs.AccessCoin(tx.vin[0].prevout).out.scriptPubKey;
        for (unsigned int i = 1; i < tx.vin.size(); ++i) {
            const Coin& coin = inputs.AccessCoin(tx.vin[i].prevout);
            if (coin.out.scriptPubKey != scriptPubKey)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputdest-invaliduniform");
        }
        for (unsigned int i = 0; i < tx.vout.size(); ++i) {
            const CTxOut &out = tx.vout[i];
            if (out.nValue != 0 && out.scriptPubKey != scriptPubKey)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-outputdest-invaliduniform");
        }
    }

    // Check for bind plotter fee and unbind plotter limit
    if (tx.IsUniform() && nSpendHeight >= params.BHDIP006CheckRelayHeight) {
        if (tx.vin.size() == 1 && tx.vout.size() == 1) {
            // Unbind & Withdraw
            const Coin& coin = inputs.AccessCoin(tx.vin[0].prevout);
            if (!coin.extraData && nSpendHeight >= params.BHDIP007Height)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-invaliduniform-unlock");
            if (coin.extraData && coin.extraData->type == DATACARRIER_TYPE_BINDPLOTTER) {
                if (fStrictCheckLimit && static_cast<int>(coin.nHeight) == nSpendHeight)
                    return state.Invalid(false, REJECT_INVALID, "bad-unbindplotter-strict-limit");
                if (nSpendHeight < GetUnbindPlotterLimitHeight(CBindPlotterInfo(tx.vin[0].prevout, coin), prevInputs, params))
                    return state.Invalid(false, REJECT_INVALID, "bad-unbindplotter-limit");
            }
        } else {
            // Bind & Rental
            CDatacarrierPayloadRef payload = ExtractTransactionDatacarrier(tx, nSpendHeight);
            if (nSpendHeight >= params.BHDIP007Height && !payload)
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-invaliduniform-type");

            // Bind plotter
            if (payload && payload->type == DATACARRIER_TYPE_BINDPLOTTER && nSpendHeight >= params.BHDIP006LimitBindPlotterHeight) {
                // Low fee
                if (txfee_aux < PROTOCOL_BINDPLOTTER_MINFEE)
                    return state.DoS(100, false, REJECT_INVALID, "bad-bindplotter-lowfee");

                // Limit bind by last
                const CBindPlotterInfo lastBindInfo = prevInputs.GetLastBindPlotterInfo(BindPlotterPayload::As(payload)->GetId());
                if (!lastBindInfo.outpoint.IsNull()) {
                    // Forbidden self-packaging change active bind tx
                    if (lastBindInfo.valid && !generatorAccountID.IsNull() && generatorAccountID == ExtractAccountID(tx.vout[0].scriptPubKey))
                        return state.DoS(100, false, REJECT_INVALID, "bad-bindplotter-selfpackaging");

                    if (nSpendHeight < GetBindPlotterLimitHeight(nSpendHeight, lastBindInfo, params)) {
                        // Change bind require high transaction fee. Diff reward between full-balance and low-balance.
                        // 23.75 - 7.5 = 16.25
                        CAmount diffReward = (GetBlockSubsidy(nSpendHeight, params) * (params.BHDIP001FundRoyaltyForLowMortgage - params.BHDIP001FundRoyaltyForFullMortgage)) / 1000;
                        if (txfee_aux < diffReward + PROTOCOL_BINDPLOTTER_MINFEE)
                            return state.Invalid(false, REJECT_INVALID, "bad-bindplotter-limitlowfee");

                        // Only pay small transaction fee to miner. Other fee to black hole
                        txfee_aux -= diffReward;
                    }
                }
            }
        }
    }

    txfee = txfee_aux;
    return true;
}

bool Consensus::CheckTxInputs(const CTransaction& tx, const CCoinsViewCache& inputs, const CCoinsViewCache& prevInputs,
    int nSpendHeight, const CAccountID& generatorAccountID, bool fStrictCheckLimit, const Consensus::Params& params)
{
    CValidationState state;
    CAmount txfee;
    return Consensus::CheckTxInputs(tx, state, inputs, prevInputs, nSpendHeight, txfee, generatorAccountID, fStrictCheckLimit, params);
}

int Consensus::GetBindPlotterLimitHeight(int nBindHeight, const CBindPlotterInfo& lastBindInfo, const Consensus::Params& params)
{
    assert(!lastBindInfo.outpoint.IsNull() && lastBindInfo.nHeight >= 0);
    assert(nBindHeight > lastBindInfo.nHeight);

    if (nBindHeight < params.BHDIP006LimitBindPlotterHeight)
        return std::max(params.BHDIP006Height, lastBindInfo.nHeight + 1);

    // Checking range [nEvalBeginHeight, nEvalEndHeight]
    const int nEvalBeginHeight = std::max(nBindHeight - params.nCapacityEvalWindow, params.BHDIP001StartMingingHeight + 1);
    const int nEvalEndHeight = nBindHeight - 1;

    // Mined block in <EvalWindow>, next block unlimit
    for (int nHeight = nEvalBeginHeight; nHeight <= nEvalEndHeight; nHeight++) {
        if (chainActive[nHeight]->nPlotterId == lastBindInfo.plotterId)
            return std::max(nHeight, lastBindInfo.nHeight) + 1;
    }

    // Participator mined after bind require lock <EvalWindow>
    const int nBeginMiningHeight = lastBindInfo.nHeight;
    const int nEndMiningHeight = std::min(lastBindInfo.nHeight + params.nCapacityEvalWindow, nEvalEndHeight);
    for (int nHeight = nBeginMiningHeight; nHeight <= nEndMiningHeight; nHeight++) {
        if (chainActive[nHeight]->generatorAccountID == lastBindInfo.accountID)
            return lastBindInfo.nHeight + params.nCapacityEvalWindow;
    }

    return lastBindInfo.nHeight + 1;
}

int Consensus::GetUnbindPlotterLimitHeight(const CBindPlotterInfo& bindInfo, const CCoinsViewCache& inputs, const Consensus::Params& params)
{
    assert(!bindInfo.outpoint.IsNull() && bindInfo.valid && bindInfo.nHeight >= 0);

    const int nSpendHeight = GetSpendHeight(inputs);
    assert(nSpendHeight >= bindInfo.nHeight);
    if (nSpendHeight < params.BHDIP006CheckRelayHeight)
        return std::max(params.BHDIP006Height, bindInfo.nHeight + 1);

    // Checking range [nEvalBeginHeight, nEvalEndHeight]
    const int nEvalBeginHeight = std::max(nSpendHeight - params.nCapacityEvalWindow, params.BHDIP001StartMingingHeight + 1);
    const int nEvalEndHeight = nSpendHeight - 1;

    // 2.5%, Large capacity unlimit
    for (int height = nEvalBeginHeight + 1, nMinedBlockCount = 0; height <= nEvalEndHeight; height++) {
        if (chainActive[height]->nPlotterId == bindInfo.plotterId) {
            if (++nMinedBlockCount > params.nCapacityEvalWindow / 40)
                return std::max(std::min(height, bindInfo.nHeight + params.nCapacityEvalWindow), bindInfo.nHeight);
        }
    }

    if (nSpendHeight < params.BHDIP006LimitBindPlotterHeight) {
        //! Issues: Infinitely +<EvalWindow> when mine to any address
        // Delay unbind EvalWindow blocks when mined block
        for (int nHeight = nEvalEndHeight; nHeight > nEvalBeginHeight; nHeight--) {
            if (chainActive[nHeight]->nPlotterId == bindInfo.plotterId)
                return nHeight + params.nCapacityEvalWindow;
        }
    } else if (nSpendHeight < params.BHDIP007Height) {
        //! Issues: Infinitely +<EvalWindow>
        const CBindPlotterInfo activeBindInfo = inputs.GetChangeBindPlotterInfo(bindInfo, true);
        assert(!activeBindInfo.outpoint.IsNull() && activeBindInfo.valid && activeBindInfo.nHeight >= 0);
        assert(activeBindInfo.nHeight >= bindInfo.nHeight);

        // Checking range [nBeginMiningHeight, nEndMiningHeight]
        const int nBeginMiningHeight = std::max(nEvalBeginHeight, bindInfo.nHeight);
        const int nEndMiningHeight = (bindInfo.outpoint == activeBindInfo.outpoint) ? nEvalEndHeight : activeBindInfo.nHeight;

        // Last mining in current wallet will lock +<EvalWindow>
        for (int nHeight = nEndMiningHeight; nHeight >= nBeginMiningHeight; nHeight--) {
            CBlockIndex *pindex = chainActive[nHeight];
            if (pindex->generatorAccountID == bindInfo.accountID && pindex->nPlotterId == bindInfo.plotterId)
                return nHeight + params.nCapacityEvalWindow;
        }

        // Participate mining lock <EvalWindow>
        for (int nHeight = nEndMiningHeight; nHeight >= nBeginMiningHeight; nHeight--) {
            if (chainActive[nHeight]->generatorAccountID == bindInfo.accountID)
                return bindInfo.nHeight + params.nCapacityEvalWindow;
        }
    } else {
        // Participate mining lock <EvalWindow>
        const CBindPlotterInfo changeBindInfo = inputs.GetChangeBindPlotterInfo(bindInfo, false);
        assert(!changeBindInfo.outpoint.IsNull() && changeBindInfo.nHeight >= 0);
        assert(changeBindInfo.nHeight >= bindInfo.nHeight);
        assert(nSpendHeight >= changeBindInfo.nHeight);

        // Checking range [nBeginMiningHeight, nEndMiningHeight]
        const int nBeginMiningHeight = bindInfo.nHeight;
        const int nEndMiningHeight = (bindInfo.outpoint == changeBindInfo.outpoint) ? nEvalEndHeight : changeBindInfo.nHeight;
        for (int nHeight = nBeginMiningHeight; nHeight <= nEndMiningHeight; nHeight++) {
            if (chainActive[nHeight]->generatorAccountID == bindInfo.accountID)
                return bindInfo.nHeight + params.nCapacityEvalWindow;
        }
    }

    return bindInfo.nHeight + 1;
}
