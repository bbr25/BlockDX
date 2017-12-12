//*****************************************************************************
//*****************************************************************************

#include "xbridgeexchange.h"
#include "xbridgeapp.h"
#include "util/logger.h"
#include "util/settings.h"
#include "util/xutil.h"
#include "bitcoinrpcconnector.h"
#include "activeservicenode.h"
#include "chainparamsbase.h"

#include <algorithm>

//******************************************************************************
//******************************************************************************
namespace xbridge
{

//*****************************************************************************
//*****************************************************************************
Exchange::Exchange()
{
}

//*****************************************************************************
//*****************************************************************************
Exchange::~Exchange()
{
}

//*****************************************************************************
//*****************************************************************************
// static
Exchange & Exchange::instance()
{
    static Exchange e;
    return e;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::init()
{
    if (!settings().isExchangeEnabled())
    {
        // disabled
        return true;
    }

    Settings & s = settings();

    std::vector<std::string> wallets = s.exchangeWallets();
    for (std::vector<std::string>::iterator i = wallets.begin(); i != wallets.end(); ++i)
    {
        std::string label      = s.get<std::string>(*i + ".Title");
        std::string address    = s.get<std::string>(*i + ".Address");
        std::string ip         = s.get<std::string>(*i + ".Ip");
        std::string port       = s.get<std::string>(*i + ".Port");
        std::string user       = s.get<std::string>(*i + ".Username");
        std::string passwd     = s.get<std::string>(*i + ".Password");
        uint64_t    minAmount  = s.get<uint64_t>(*i + ".MinimumAmount", 0);
        uint64_t    dustAmount = s.get<uint64_t>(*i + ".DustAmount", 0);
        uint32_t    txVersion  = s.get<uint32_t>(*i + ".TxVersion", 1);
        uint64_t    minTxFee   = s.get<uint64_t>(*i + ".MinTxFee", 0);
        uint64_t    feePerByte = s.get<uint64_t>(*i + ".FeePerByte", 200);


        if (/*address.empty() || */ip.empty() || port.empty() ||
                user.empty() || passwd.empty())
        {
            LOG() << "read wallet " << *i << " with empty parameters>";
            continue;
        }

        WalletParam & wp = m_wallets[*i];
        wp.currency   = *i;
        wp.title      = label;
        wp.m_ip         = ip;
        wp.m_port       = port;
        wp.m_user       = user;
        wp.m_passwd     = passwd;
        wp.m_minAmount  = minAmount;
        wp.dustAmount = dustAmount;
        wp.txVersion  = txVersion;
        wp.minTxFee   = minTxFee;
        wp.feePerByte = feePerByte;

        LOG() << "read wallet " << *i << " \"" << label << "\" address <" << address << ">";
    }

    if (isEnabled())
    {
        LOG() << "exchange enabled";
    }

    if (isStarted())
    {
        LOG() << "exchange started";
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::isEnabled()
{
    return ((m_wallets.size() > 0) && fServiceNode);
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::isStarted()
{
    return (isEnabled() && (activeServicenode.status == ACTIVE_SERVICENODE_STARTED));
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::haveConnectedWallet(const std::string & walletName)
{
    return m_wallets.count(walletName) > 0;
}

//*****************************************************************************
//*****************************************************************************
std::vector<std::string> Exchange::connectedWallets() const
{
    std::vector<std::string> list;
    for (const auto & wallet : m_wallets)
    {
        list.push_back(wallet.first);
    }
    return list;
}

//*****************************************************************************
//*****************************************************************************
//std::vector<unsigned char> XBridgeExchange::walletAddress(const std::string & walletName)
//{
//    if (!m_wallets.count(walletName))
//    {
//        ERR() << "reqyest address for unknown wallet <" << walletName
//              << ">" << __FUNCTION__;
//        return std::vector<unsigned char>();
//    }

//    return m_wallets[walletName].address;
//}

//*****************************************************************************
//*****************************************************************************
bool Exchange::createTransaction(const uint256                    & id,
                                        const std::vector<unsigned char> & sourceAddr,
                                        const std::string                & sourceCurrency,
                                        const uint64_t                   & sourceAmount,
                                        const std::vector<unsigned char> & destAddr,
                                        const std::string                & destCurrency,
                                        const uint64_t                   & destAmount,
                                        uint256                          & pendingId,
                                        bool                             & isCreated)
{
    DEBUG_TRACE();

    isCreated = false;

    if (!haveConnectedWallet(sourceCurrency) || !haveConnectedWallet(destCurrency))
    {
        LOG() << "no active wallet for transaction "
              << util::base64_encode(std::string((char *)id.begin(), 32));
        return false;
    }

    const WalletParam & wp  = m_wallets[sourceCurrency];
    const WalletParam & wp2 = m_wallets[destCurrency];

    // check amounts
    {
        if (wp.m_minAmount && wp.m_minAmount > sourceAmount)
        {
            LOG() << "tx "
                  << util::base64_encode(std::string((char *)id.begin(), 32))
                  << " rejected because sourceAmount less than minimum payment";
            return false;
        }
        if (wp2.m_minAmount && wp2.m_minAmount > destAmount)
        {
            LOG() << "tx "
                  << util::base64_encode(std::string((char *)id.begin(), 32))
                  << " rejected because destAmount less than minimum payment";
            return false;
        }
    }

    TransactionPtr tr(new xbridge::Transaction(id,
                                                    sourceAddr,
                                                    sourceCurrency, sourceAmount,
                                                    destAddr,
                                                    destCurrency, destAmount));

    LOG() << tr->hash1().ToString();
    LOG() << tr->hash2().ToString();

    if (!tr->isValid())
    {
        return false;
    }

    uint256 h = tr->hash2();
    pendingId = h;

    {
        boost::mutex::scoped_lock l(m_pendingTransactionsLock);

        if (!m_pendingTransactions.count(h))
        {
            // new transaction
            isCreated = true;
            pendingId = h = tr->hash1();
            m_pendingTransactions[h] = tr;
        }
        else
        {
            boost::mutex::scoped_lock l2(m_pendingTransactions[h]->m_lock);

            // found, check if expired
            if (m_pendingTransactions[h]->isExpired())
            {
                // if expired - delete old transaction
                m_pendingTransactions.erase(h);

                // create new
                pendingId = h = tr->hash1();
                m_pendingTransactions[h] = tr;
            }
        }
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::acceptTransaction(const uint256                    & id,
                                        const std::vector<unsigned char> & sourceAddr,
                                        const std::string                & sourceCurrency,
                                        const uint64_t                   & sourceAmount,
                                        const std::vector<unsigned char> & destAddr,
                                        const std::string                & destCurrency,
                                        const uint64_t                   & destAmount,
                                        uint256                          & transactionId)
{
    DEBUG_TRACE();

    if (!haveConnectedWallet(sourceCurrency) || !haveConnectedWallet(destCurrency))
    {
        LOG() << "no active wallet for transaction "
              << util::base64_encode(std::string((char *)id.begin(), 32));
        return false;
    }

    TransactionPtr tr(new xbridge::Transaction(id,
                                                    sourceAddr,
                                                    sourceCurrency, sourceAmount,
                                                    destAddr,
                                                    destCurrency, destAmount));

    transactionId = id;

    if (!tr->isValid())
    {
        return false;
    }

    uint256 h = tr->hash2();

    TransactionPtr tmp;

    {
        boost::mutex::scoped_lock l(m_pendingTransactionsLock);

        if (!m_pendingTransactions.count(h))
        {
            // no pending
            return false;
        }
        else
        {
            boost::mutex::scoped_lock l2(m_pendingTransactions[h]->m_lock);

            // found, check if expired
            if (m_pendingTransactions[h]->isExpired())
            {
                // if expired - delete old transaction
                m_pendingTransactions.erase(h);

                // create new
                h = tr->hash1();
                m_pendingTransactions[h] = tr;
            }
            else
            {
                // try join with existing transaction
                if (!m_pendingTransactions[h]->tryJoin(tr))
                {
                    LOG() << "transaction not joined";
                    // return false;

                    // create new transaction
                    h = tr->hash1();
                    m_pendingTransactions[h] = tr;
                }
                else
                {
                    LOG() << "transactions joined, new id <" << tr->id().GetHex() << ">";

                    tmp = m_pendingTransactions[h];
                }
            }
        }
    }

    if (tmp)
    {
        // move to transactions
        {
            boost::mutex::scoped_lock l(m_transactionsLock);
            m_transactions[tmp->id()] = tmp;
        }
        {
            boost::mutex::scoped_lock l(m_pendingTransactionsLock);
            m_pendingTransactions.erase(h);
        }

        transactionId = tmp->id();
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::deletePendingTransactions(const uint256 & id)
{
    boost::mutex::scoped_lock l(m_pendingTransactionsLock);

    LOG() << "delete pending transaction <" << id.GetHex() << ">";

    addToTransactionsHistory(id);
    m_pendingTransactions.erase(id);
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::deleteTransaction(const uint256 & id)
{
    boost::mutex::scoped_lock l(m_transactionsLock);

    LOG() << "delete transaction <" << id.GetHex() << ">";

    addToTransactionsHistory(id);
    m_transactions.erase(id);
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::updateTransactionWhenHoldApplyReceived(const TransactionPtr & tx,
                                                             const std::vector<unsigned char> & from)
{
    if (tx->increaseStateCounter(xbridge::Transaction::trJoined, from) == xbridge::Transaction::trHold)
    {
        return true;
    }

    return false;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::updateTransactionWhenInitializedReceived(const TransactionPtr &tx,
                                                               const std::vector<unsigned char> & from,
                                                               const uint256 & datatxid,
                                                               const std::vector<unsigned char> & pk)
{
    if (!tx->setKeys(from, datatxid, pk))
    {
        // wtf?
        LOG() << "unknown sender address for transaction, id <" << tx->id().GetHex() << ">";
        return false;
    }

    if (tx->increaseStateCounter(xbridge::Transaction::trHold, from) == xbridge::Transaction::trInitialized)
    {
        return true;
    }

    return false;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::updateTransactionWhenCreatedReceived(const TransactionPtr & tx,
                                                           const std::vector<unsigned char> & from,
                                                           const std::string & binTxId,
                                                           const std::vector<unsigned char> & innerScript)
{
    if (!tx->setBinTxId(from, binTxId, innerScript))
    {
        // wtf?
        LOG() << "unknown sender address for transaction, id <" << tx->id().GetHex() << ">";
        return false;
    }

    if (tx->increaseStateCounter(xbridge::Transaction::trInitialized, from) == xbridge::Transaction::trCreated)
    {
        return true;
    }

    return false;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::updateTransactionWhenConfirmedReceived(const TransactionPtr & tx,
                                                             const std::vector<unsigned char> & from)
{
    // update transaction state
    if (tx->increaseStateCounter(xbridge::Transaction::trCreated, from) == xbridge::Transaction::trFinished)
    {
        return true;
    }

    return false;
}

//*****************************************************************************
//*****************************************************************************
bool Exchange::updateTransaction(const uint256 & /*hash*/)
{
    LOG() << "not implemented";
    return true;

//    // DEBUG_TRACE();

//    // store
//    m_walletTransactions.insert(hash);

//    // check unconfirmed
//    boost::mutex::scoped_lock l (m_unconfirmedLock);
//    if (m_unconfirmed.size())
//    {
//        for (std::map<uint256, uint256>::iterator i = m_unconfirmed.begin(); i != m_unconfirmed.end();)
//        {
//            if (m_walletTransactions.count(i->first))
//            {
//                LOG() << "confirm transaction, id <" << i->second.GetHex()
//                      << "> hash <" << i->first.GetHex() << ">";

//                XBridgeTransactionPtr tr = transaction(i->second);
//                boost::mutex::scoped_lock l(tr->m_lock);

//                tr->confirm(i->first);

//                i = m_unconfirmed.erase(i);
//            }
//            else
//            {
//                ++i;
//            }
//        }
//    }


//    uint256 txid;

//    boost::mutex::scoped_lock l (m_unconfirmedLock);
//    if (m_unconfirmed.count(hash))
//    {
//        txid = m_unconfirmed[hash];

//        LOG() << "confirm transaction, id <"
//              << util::base64_encode(std::string((char *)(txid.begin()), 32))
//              << "> hash <"
//              << util::base64_encode(std::string((char *)(hash.begin()), 32))
//              << ">";

//        m_unconfirmed.erase(hash);
//    }

//    if (txid != uint256())
//    {
//        XBridgeTransactionPtr tr = transaction(txid);
//        boost::mutex::scoped_lock l(tr->m_lock);

//        tr->confirm(hash);
//    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
const TransactionPtr Exchange::transaction(const uint256 & hash)
{
    {
        boost::mutex::scoped_lock l(m_transactionsLock);

        if (m_transactions.count(hash))
        {
            return m_transactions[hash];
        }
        else
        {
            // unknown transaction
            LOG() << "unknown transaction, id <" << hash.GetHex() << ">";
        }
    }

    // TODO not search in pending transactions
//    {
//        boost::mutex::scoped_lock l(m_pendingTransactionsLock);

//        if (m_pendingTransactions.count(hash))
//        {
//            return m_pendingTransactions[hash];
//        }
//    }

    // return XBridgeTransaction::trInvalid;
    return TransactionPtr(new xbridge::Transaction);
}

//*****************************************************************************
//*****************************************************************************
const TransactionPtr Exchange::pendingTransaction(const uint256 & hash)
{
    {
        boost::mutex::scoped_lock l(m_pendingTransactionsLock);

        if (m_pendingTransactions.count(hash))
        {
            return m_pendingTransactions[hash];
        }
        else
        {
            // unknown transaction
            LOG() << "unknown pending transaction, id <" << hash.GetHex() << ">";
        }
    }

    // return XBridgeTransaction::trInvalid;
    return TransactionPtr(new xbridge::Transaction);
}

//*****************************************************************************
//*****************************************************************************
std::list<TransactionPtr> Exchange::pendingTransactions() const
{
    boost::mutex::scoped_lock l(m_pendingTransactionsLock);

    std::list<TransactionPtr> list;

    for (std::map<uint256, TransactionPtr>::const_iterator i = m_pendingTransactions.begin();
         i != m_pendingTransactions.end(); ++i)
    {
        list.push_back(i->second);
    }

    return list;
}

//*****************************************************************************
//*****************************************************************************
std::list<TransactionPtr> Exchange::transactions(bool onlyFinished) const
{
    boost::mutex::scoped_lock l(m_transactionsLock);

    std::list<TransactionPtr> list;

    for (std::map<uint256, TransactionPtr>::const_iterator i = m_transactions.begin(); i != m_transactions.end(); ++i)
    {
        if (!onlyFinished)
        {
            list.push_back(i->second);
        }
        else if (i->second->isExpired() ||
                 !i->second->isValid() ||
                 i->second->isFinished() ||
                 i->second->state() == xbridge::Transaction::trConfirmed)
        {
            list.push_back(i->second);
        }
    }

    return list;
}

//*****************************************************************************
//*****************************************************************************
std::list<TransactionPtr> Exchange::transactions() const
{
    return transactions(false);
}

//*****************************************************************************
//*****************************************************************************
std::list<TransactionPtr> Exchange::finishedTransactions() const
{
    return transactions(true);
}

//*****************************************************************************
//*****************************************************************************
std::list<TransactionPtr> Exchange::transactionsHistory() const
{
    boost::mutex::scoped_lock l(m_transactionsHistoryLock);

    std::list<TransactionPtr> list;

    for (std::map<uint256, TransactionPtr>::const_iterator i = m_transactionsHistory.begin(); i != m_transactionsHistory.end(); ++i)
    {
        list.push_back(i->second);
    }

    return list;
}

//*****************************************************************************
//*****************************************************************************
void Exchange::addToTransactionsHistory(const uint256 &id)
{
    boost::mutex::scoped_lock l(m_transactionsHistoryLock);

    if (m_transactions.count(id))
    {
        m_transactionsHistory[id] = m_transactions[id];
    }
    else if(m_pendingTransactions.count(id))
    {
        m_transactionsHistory[id] = m_pendingTransactions[id];
    }

    LOG() << "Nothing to add to transactions history";
}

} // namespace xbridge

