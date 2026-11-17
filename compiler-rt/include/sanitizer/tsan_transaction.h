#ifndef SANITIZER_TSAN_TRANSACTION_H
#define SANITIZER_TSAN_TRANSACTION_H

#ifdef __cplusplus
extern "C" {
#endif

void __tsan_transaction_begin(void);
void __tsan_transaction_end(void);

#ifdef __cplusplus
}

namespace atomsan {

class TransactionScope {
public:
  TransactionScope() { __tsan_transaction_begin(); }
  ~TransactionScope() { __tsan_transaction_end(); }

  TransactionScope(const TransactionScope &) = delete;
  TransactionScope &operator=(const TransactionScope &) = delete;
};

} // namespace atomsan
#endif

#endif
