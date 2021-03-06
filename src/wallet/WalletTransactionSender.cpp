// Copyright (c) 2011-2015 The Cryptonote developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// epee
#include "misc_language.h"

#include "cryptonote_core/account.h"
#include "cryptonote_core/cryptonote_format_utils.h"

#include "WalletTransactionSender.h"
#include "WalletUtils.h"

#include "cryptonote_core/cryptonote_basic_impl.h"

#include <random>

namespace {

using namespace CryptoNote;

uint64_t countNeededMoney(uint64_t fee, const std::vector<CryptoNote::Transfer>& transfers) {
  uint64_t needed_money = fee;
  for (auto& transfer: transfers) {
    CryptoNote::throwIf(transfer.amount == 0, cryptonote::error::ZERO_DESTINATION);
    CryptoNote::throwIf(transfer.amount < 0, cryptonote::error::WRONG_AMOUNT);

    needed_money += transfer.amount;
    CryptoNote::throwIf(static_cast<int64_t>(needed_money) < transfer.amount, cryptonote::error::SUM_OVERFLOW);
  }

  return needed_money;
}

void createChangeDestinations(const cryptonote::AccountPublicAddress& address, uint64_t neededMoney, uint64_t foundMoney, cryptonote::tx_destination_entry& changeDts) {
  if (neededMoney < foundMoney) {
    changeDts.addr = address;
    changeDts.amount = foundMoney - neededMoney;
  }
}

void constructTx(const cryptonote::account_keys keys, const std::vector<cryptonote::tx_source_entry>& sources, const std::vector<cryptonote::tx_destination_entry>& splittedDests,
    const std::string& extra, uint64_t unlockTimestamp, uint64_t sizeLimit, cryptonote::Transaction& tx) {
  std::vector<uint8_t> extraVec;
  extraVec.reserve(extra.size());
  std::for_each(extra.begin(), extra.end(), [&extraVec] (const char el) { extraVec.push_back(el);});

  bool r = cryptonote::construct_tx(keys, sources, splittedDests, extraVec, tx, unlockTimestamp);
  CryptoNote::throwIf(!r, cryptonote::error::INTERNAL_WALLET_ERROR);
  CryptoNote::throwIf(cryptonote::get_object_blobsize(tx) >= sizeLimit, cryptonote::error::TRANSACTION_SIZE_TOO_BIG);
}

void fillTransactionHash(const cryptonote::Transaction& tx, CryptoNote::TransactionHash& hash) {
  crypto::hash h = cryptonote::get_transaction_hash(tx);
  memcpy(hash.data(), reinterpret_cast<const uint8_t *>(&h), hash.size());
}

std::shared_ptr<WalletEvent> makeCompleteEvent(WalletUserTransactionsCache& transactionCache, size_t transactionId, std::error_code ec) {
  transactionCache.updateTransactionSendingState(transactionId, ec);
  return std::make_shared<WalletSendTransactionCompletedEvent>(transactionId, ec);
}

} //namespace

namespace CryptoNote {

WalletTransactionSender::WalletTransactionSender(const cryptonote::Currency& currency, WalletUserTransactionsCache& transactionsCache, cryptonote::account_keys keys, ITransfersContainer& transfersContainer) :
  m_currency(currency),
  m_transactionsCache(transactionsCache),
  m_isStoping(false),
  m_keys(keys),
  m_transferDetails(transfersContainer),
  m_upperTransactionSizeLimit(m_currency.blockGrantedFullRewardZone() * 2 - m_currency.minerTxBlobReservedSize()) {}

void WalletTransactionSender::stop() {
  m_isStoping = true;
}

bool WalletTransactionSender::validateDestinationAddress(const std::string& address) {
  cryptonote::AccountPublicAddress ignore;
  return m_currency.parseAccountAddressString(address, ignore);
}

void WalletTransactionSender::validateTransfersAddresses(const std::vector<Transfer>& transfers) {
  for (const Transfer& tr: transfers) {
    if (!validateDestinationAddress(tr.address)) {
      throw std::system_error(make_error_code(cryptonote::error::BAD_ADDRESS));
    }
  }
}

std::shared_ptr<WalletRequest> WalletTransactionSender::makeSendRequest(TransactionId& transactionId, std::deque<std::shared_ptr<WalletEvent> >& events,
    const std::vector<Transfer>& transfers, uint64_t fee, const std::string& extra, uint64_t mixIn, uint64_t unlockTimestamp) {

  using namespace cryptonote;

  throwIf(transfers.empty(), cryptonote::error::ZERO_DESTINATION);
  validateTransfersAddresses(transfers);
  uint64_t neededMoney = countNeededMoney(fee, transfers);

  std::shared_ptr<SendTransactionContext> context = std::make_shared<SendTransactionContext>();

  context->foundMoney = selectTransfersToSend(neededMoney, 0 == mixIn, context->dustPolicy.dustThreshold, context->selectedTransfers);
  throwIf(context->foundMoney < neededMoney, cryptonote::error::WRONG_AMOUNT);

  transactionId = m_transactionsCache.addNewTransaction(neededMoney, fee, extra, transfers, unlockTimestamp);
  context->transactionId = transactionId;
  context->mixIn = mixIn;

  if(context->mixIn) {
    std::shared_ptr<WalletRequest> request = makeGetRandomOutsRequest(context);
    return request;
  }

  return doSendTransaction(context, events);
}

std::shared_ptr<WalletRequest> WalletTransactionSender::makeGetRandomOutsRequest(std::shared_ptr<SendTransactionContext> context) {
  uint64_t outsCount = context->mixIn + 1;// add one to make possible (if need) to skip real output key
  std::vector<uint64_t> amounts;

  for (const auto& td : context->selectedTransfers) {
    amounts.push_back(td.amount);
  }

  return std::make_shared<WalletGetRandomOutsByAmountsRequest>(amounts, outsCount, context, std::bind(&WalletTransactionSender::sendTransactionRandomOutsByAmount,
      this, context, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
}

void WalletTransactionSender::sendTransactionRandomOutsByAmount(std::shared_ptr<SendTransactionContext> context, std::deque<std::shared_ptr<WalletEvent> >& events,
    boost::optional<std::shared_ptr<WalletRequest> >& nextRequest, std::error_code ec) {
  
  if (m_isStoping) {
    ec = make_error_code(cryptonote::error::TX_CANCELLED);
  }

  if (ec) {
    events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, ec));
    return;
  }

  auto scanty_it = std::find_if(context->outs.begin(), context->outs.end(), 
    [&] (cryptonote::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& out) {return out.outs.size() < context->mixIn;});

  if (scanty_it != context->outs.end()) {
    events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, make_error_code(cryptonote::error::MIXIN_COUNT_TOO_BIG)));
    return;
  }

  std::shared_ptr<WalletRequest> req = doSendTransaction(context, events);
  if (req)
    nextRequest = req;
}

std::shared_ptr<WalletRequest> WalletTransactionSender::doSendTransaction(std::shared_ptr<SendTransactionContext> context, std::deque<std::shared_ptr<WalletEvent> >& events) {
  if (m_isStoping) {
    events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, make_error_code(cryptonote::error::TX_CANCELLED)));
    return std::shared_ptr<WalletRequest>();
  }

  try
  {
    TransactionInfo& transaction = m_transactionsCache.getTransaction(context->transactionId);

    std::vector<cryptonote::tx_source_entry> sources;
    prepareInputs(context->selectedTransfers, context->outs, sources, context->mixIn);

    cryptonote::tx_destination_entry changeDts = AUTO_VAL_INIT(changeDts);
    uint64_t totalAmount = -transaction.totalAmount;
    createChangeDestinations(m_keys.m_account_address, totalAmount, context->foundMoney, changeDts);

    std::vector<cryptonote::tx_destination_entry> splittedDests;
    splitDestinations(transaction.firstTransferId, transaction.transferCount, changeDts, context->dustPolicy, splittedDests);

    cryptonote::Transaction tx;
    constructTx(m_keys, sources, splittedDests, transaction.extra, transaction.unlockTime, m_upperTransactionSizeLimit, tx);

    fillTransactionHash(tx, transaction.hash);

    m_transactionsCache.updateTransaction(context->transactionId, tx, totalAmount, context->selectedTransfers);

    notifyBalanceChanged(events);
   
    return std::make_shared<WalletRelayTransactionRequest>(tx, std::bind(&WalletTransactionSender::relayTransactionCallback, this, context,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
  }
  catch(std::system_error& ec) {
    events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, ec.code()));
  }
  catch(std::exception&) {
    events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, make_error_code(cryptonote::error::INTERNAL_WALLET_ERROR)));
  }

  return std::shared_ptr<WalletRequest>();
}

void WalletTransactionSender::relayTransactionCallback(std::shared_ptr<SendTransactionContext> context, std::deque<std::shared_ptr<WalletEvent> >& events,
                                                        boost::optional<std::shared_ptr<WalletRequest> >& nextRequest, std::error_code ec) {
  if (m_isStoping) {
    return;
  }

  events.push_back(makeCompleteEvent(m_transactionsCache, context->transactionId, ec));
}


void WalletTransactionSender::splitDestinations(TransferId firstTransferId, size_t transfersCount, const cryptonote::tx_destination_entry& changeDts,
                                                const TxDustPolicy& dustPolicy, std::vector<cryptonote::tx_destination_entry>& splittedDests) {
  uint64_t dust = 0;

  digitSplitStrategy(firstTransferId, transfersCount, changeDts, dustPolicy.dustThreshold, splittedDests, dust);

  throwIf(dustPolicy.dustThreshold < dust, cryptonote::error::INTERNAL_WALLET_ERROR);
  if (0 != dust && !dustPolicy.addToFee) {
    splittedDests.push_back(cryptonote::tx_destination_entry(dust, dustPolicy.addrForDust));
  }
}


void WalletTransactionSender::digitSplitStrategy(TransferId firstTransferId, size_t transfersCount,
  const cryptonote::tx_destination_entry& change_dst, uint64_t dust_threshold,
  std::vector<cryptonote::tx_destination_entry>& splitted_dsts, uint64_t& dust) {
  splitted_dsts.clear();
  dust = 0;

  for (TransferId idx = firstTransferId; idx < firstTransferId + transfersCount; ++idx) {
    Transfer& de = m_transactionsCache.getTransfer(idx);

    cryptonote::AccountPublicAddress addr;
    if (!m_currency.parseAccountAddressString(de.address, addr)) {
      throw std::system_error(make_error_code(cryptonote::error::BAD_ADDRESS));
    }

    cryptonote::decompose_amount_into_digits(de.amount, dust_threshold,
      [&](uint64_t chunk) { splitted_dsts.push_back(cryptonote::tx_destination_entry(chunk, addr)); },
      [&](uint64_t a_dust) { splitted_dsts.push_back(cryptonote::tx_destination_entry(a_dust, addr)); } );
  }

  cryptonote::decompose_amount_into_digits(change_dst.amount, dust_threshold,
    [&](uint64_t chunk) { splitted_dsts.push_back(cryptonote::tx_destination_entry(chunk, change_dst.addr)); },
    [&](uint64_t a_dust) { dust = a_dust; } );
}


void WalletTransactionSender::prepareInputs(
  const std::list<TransactionOutputInformation>& selectedTransfers,
  std::vector<cryptonote::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount>& outs,
  std::vector<cryptonote::tx_source_entry>& sources, uint64_t mixIn) {

  size_t i = 0;

  for (const auto& td: selectedTransfers) {
    sources.resize(sources.size()+1);
    cryptonote::tx_source_entry& src = sources.back();

    src.amount = td.amount;

    //paste mixin transaction
    if(outs.size()) {
      outs[i].outs.sort([](const cryptonote::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry& a, const cryptonote::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry& b){return a.global_amount_index < b.global_amount_index;});
      for (auto& daemon_oe: outs[i].outs) {
        if(td.globalOutputIndex == daemon_oe.global_amount_index)
          continue;
        cryptonote::tx_source_entry::output_entry oe;
        oe.first = daemon_oe.global_amount_index;
        oe.second = daemon_oe.out_key;
        src.outputs.push_back(oe);
        if(src.outputs.size() >= mixIn)
          break;
      }
    }

    //paste real transaction to the random index
    auto it_to_insert = std::find_if(src.outputs.begin(), src.outputs.end(), [&](const cryptonote::tx_source_entry::output_entry& a) { return a.first >= td.globalOutputIndex; });

    cryptonote::tx_source_entry::output_entry real_oe;
    real_oe.first = td.globalOutputIndex;
    real_oe.second = reinterpret_cast<const crypto::public_key&>(td.outputKey);

    auto interted_it = src.outputs.insert(it_to_insert, real_oe);

    src.real_out_tx_key = reinterpret_cast<const crypto::public_key&>(td.transactionPublicKey);
    src.real_output = interted_it - src.outputs.begin();
    src.real_output_in_tx_index = td.outputInTransaction;
    ++i;
  }
}

void WalletTransactionSender::notifyBalanceChanged(std::deque<std::shared_ptr<WalletEvent> >& events) {
  uint64_t unconfirmedOutsAmount = m_transactionsCache.unconfrimedOutsAmount();
  uint64_t change = unconfirmedOutsAmount - m_transactionsCache.unconfirmedTransactionsAmount();

  uint64_t actualBalance = m_transferDetails.balance(ITransfersContainer::IncludeKeyUnlocked) - unconfirmedOutsAmount;
  uint64_t pendingBalance = m_transferDetails.balance(ITransfersContainer::IncludeKeyNotUnlocked) + change;

  events.push_back(std::make_shared<WalletActualBalanceUpdatedEvent>(actualBalance));
  events.push_back(std::make_shared<WalletPendingBalanceUpdatedEvent>(pendingBalance));
}

namespace {

template<typename URNG, typename T>
T popRandomValue(URNG& randomGenerator, std::vector<T>& vec) {
  CHECK_AND_ASSERT_MES(!vec.empty(), T(), "Vector must be non-empty");

  std::uniform_int_distribution<size_t> distribution(0, vec.size() - 1);
  size_t idx = distribution(randomGenerator);

  T res = vec[idx];
  if (idx + 1 != vec.size()) {
    vec[idx] = vec.back();
  }
  vec.resize(vec.size() - 1);

  return res;
}

}


uint64_t WalletTransactionSender::selectTransfersToSend(uint64_t neededMoney, bool addDust, uint64_t dust, std::list<TransactionOutputInformation>& selectedTransfers) {

  std::vector<size_t> unusedTransfers;
  std::vector<size_t> unusedDust;

  std::vector<TransactionOutputInformation> outputs;
  m_transferDetails.getOutputs(outputs, ITransfersContainer::IncludeKeyUnlocked);

  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& out = outputs[i];
    if (!m_transactionsCache.isUsed(out)) {
      if (dust < out.amount)
        unusedTransfers.push_back(i);
      else
        unusedDust.push_back(i);
    }
  }

  std::default_random_engine randomGenerator(crypto::rand<std::default_random_engine::result_type>());
  bool selectOneDust = addDust && !unusedDust.empty();
  uint64_t foundMoney = 0;

  while (foundMoney < neededMoney && (!unusedTransfers.empty() || !unusedDust.empty())) {
    size_t idx;
    if (selectOneDust) {
      idx = popRandomValue(randomGenerator, unusedDust);
      selectOneDust = false;
    } else {
      idx = !unusedTransfers.empty() ? popRandomValue(randomGenerator, unusedTransfers) : popRandomValue(randomGenerator, unusedDust);
    }

    selectedTransfers.push_back(outputs[idx]);
    foundMoney += outputs[idx].amount;
  }

  return foundMoney;

}


} /* namespace CryptoNote */
