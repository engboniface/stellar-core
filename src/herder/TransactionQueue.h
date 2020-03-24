#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TxSetFrame.h"
#include "transactions/TransactionFrame.h"
#include "util/HashOfHash.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-transaction.h"

#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace medida
{
class Counter;
}

namespace stellar
{

class Application;

/**
 * This class keeps received transaction that were not yet added into ledger
 * and that are valid.
 *
 * Each account has an associated queue of transactions (with increasing
 * sequence numbers), a cached value of total fees for those transactions and
 * an age used to determine how long transaction should be kept before banning.
 *
 * After receiving transaction from network it should be added to this queue
 * by tryAdd operation. If that succeeds, it can be later removed from it in one
 * of three ways:
 * * removeAndReset() should be called after transaction is successfully
 *   included into some leger. It preserves the other pending transactions for
 *   accounts and resets the TTL for banning
 * * ban() bans a transactions (which may or may not be in the queue) and its
 *   descendants, if any, for the next few ledgers.
 * * shift() should be called after each ledger close, it bans transactions
 *   that have associated age greater or equal to pendingDepth and removes
 *   transactions that were banned for more than banDepth ledgers
 *
 * Current value of total fees, age and last sequence number of transaction in
 * queue for given account can be returned by getAccountTransactionQueueInfo.
 */
class TransactionQueue
{
  public:
    static int64_t const FEE_MULTIPLIER;

    enum class AddResult
    {
        ADD_STATUS_PENDING = 0,
        ADD_STATUS_DUPLICATE,
        ADD_STATUS_ERROR,
        ADD_STATUS_TRY_AGAIN_LATER,
        ADD_STATUS_COUNT
    };

    /*
     * Information about queue of transaction for given account. mAge and
     * mTotalFees are stored in queue, but mMaxSeq must be computed each
     * time (its O(1) anyway).
     */
    struct AccountTxQueueInfo
    {
        SequenceNumber mMaxSeq{0};
        int64_t mTotalFees{0};
        size_t mQueueSizeOps{0};
        int mAge{0};

        friend bool operator==(AccountTxQueueInfo const& x,
                               AccountTxQueueInfo const& y);
    };

    /**
     * Queue of transaction for given account. mTotalFees is a sum of all
     * feeBid() values from mTransactions. mAge is incremented each time
     * shift() is called and allows for banning transactions.
     */
    struct AccountTransactions
    {
        using Transactions = std::vector<TransactionFrameBasePtr>;

        int64_t mTotalFees{0};
        size_t mQueueSizeOps{0};
        int mAge{0};
        Transactions mTransactions;
    };

    explicit TransactionQueue(Application& app, int pendingDepth, int banDepth,
                              int poolLedgerMultiplier);

    AddResult tryAdd(TransactionFrameBasePtr tx);
    void removeAndReset(std::vector<TransactionFrameBasePtr> const& txs);
    void ban(std::vector<TransactionFrameBasePtr> const& txs);

    /**
     * Increse age of each transaction queue. If that age now is equal to
     * pendingDepth, all ot transaction on that queue are banned. Also
     * increments age for each banned transaction and if that age became equal
     * to banDepth, transaction get unbanned.
     */
    void shift();

    AccountTxQueueInfo
    getAccountTransactionQueueInfo(AccountID const& accountID) const;

    int countBanned(int index) const;
    bool isBanned(Hash const& hash) const;

    std::shared_ptr<TxSetFrame>
    toTxSet(LedgerHeaderHistoryEntry const& lcl) const;

  private:
    /**
     * Per account queue. Each queue has its own age, so it is easy to reset it
     * when transaction for given account was included in ledger. It also
     * allows for fast banning of all transaction that depend (have bigger
     * sequence number) of just-removed invalid one in ban().
     */
    using PendingTransactions =
        std::unordered_map<AccountID, AccountTransactions>;
    /**
     * Banned transactions are stored in deque of depth banDepth, so it is easy
     * to unban all transactions that were banned for long enough.
     */
    using BannedTransactions = std::deque<std::unordered_set<Hash>>;

    Application& mApp;
    int const mPendingDepth;
    std::vector<medida::Counter*> mSizeByAge;
    PendingTransactions mPendingTransactions;
    BannedTransactions mBannedTransactions;

    using FindResult = std::pair<PendingTransactions::iterator,
                                 AccountTransactions::Transactions::iterator>;
    FindResult find(TransactionFrameBasePtr const& tx);
    using ExtractResult = std::pair<PendingTransactions::iterator,
                                    std::vector<TransactionFrameBasePtr>>;
    // keepBacklog: keeps transactions succeeding tx in the account's backlog
    ExtractResult extract(TransactionFrameBasePtr const& tx, bool keepBacklog);

    AddResult canAdd(TransactionFrameBasePtr tx,
                     PendingTransactions::iterator& pendingIter,
                     AccountTransactions::Transactions::iterator& oldTxIter);

    void releaseFee(TransactionFrameBasePtr tx);

    // size of the transaction queue, in operations
    size_t mQueueSizeOps{0};
    // number of ledgers we can pool in memory
    int const mPoolLedgerMultiplier;

    size_t maxQueueSizeOps() const;

#ifdef BUILD_TESTS
  public:
    size_t
    getQueueSizeOps() const
    {
        return mQueueSizeOps;
    }
#endif
};

static const char* TX_STATUS_STRING[static_cast<int>(
    TransactionQueue::AddResult::ADD_STATUS_COUNT)] = {
    "PENDING", "DUPLICATE", "ERROR", "TRY_AGAIN_LATER"};
}
