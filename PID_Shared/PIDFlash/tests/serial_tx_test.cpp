#include "../src/serial_tx.h"
#include <cassert>
#include <cstdio>
using namespace pidflash;
int main(){
  SerialTxLedger ledger;ledger.reset(17);uint8_t bytes[]={1,2,3};unsigned writes=0;
  auto writer=[&](const uint8_t*,size_t n){++writes;return n;};
  assert(ledger.write(17,0,bytes,3,writer)==Error::Ok);
  assert(ledger.write(17,0,bytes,3,writer)==Error::Ok&&writes==1&&ledger.total()==3);
  bytes[0]=9;assert(ledger.write(17,0,bytes,3,writer)==Error::Conflict);
  assert(ledger.write(18,3,bytes,3,writer)==Error::Stale&&writes==1);
  assert(ledger.write(17,1,bytes,3,writer)==Error::Offset);
  assert(ledger.write(17,3,bytes,3,[](const uint8_t*,size_t){return SIZE_MAX;})==Error::Busy);
  assert(ledger.write(17,3,bytes,3,writer)==Error::Ok&&writes==2);
  assert(ledger.write(17,6,bytes,3,[](const uint8_t*,size_t){return size_t(1);})==Error::Timeout);
  assert(ledger.write(17,6,bytes,3,writer)==Error::Timeout&&writes==2);
  assert(ledger.write(17,7,bytes,3,writer)==Error::Conflict);
  ledger.reset(18);assert(ledger.write(18,0,bytes,3,writer)==Error::Ok&&ledger.total()==3);
  std::puts("Serial TX: duplicate ACK, Busy retry, epoch and partial-write tests passed");
}
