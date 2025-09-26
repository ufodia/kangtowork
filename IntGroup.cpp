#include <immintrin.h>
#include "IntGroup.h"

using namespace std;

IntGroup::IntGroup(int size) {
  this->size = size;
  // Use aligned allocation for better SIMD performance
  if (posix_memalign((void**)&subp, 64, size * sizeof(Int)) != 0) {
    subp = (Int *)malloc(size * sizeof(Int));
  }
}

IntGroup::~IntGroup() {
  free(subp);
}

void IntGroup::Set(Int *pts) {
  ints = pts;
}

// Compute modular inversion of the whole group
void IntGroup::ModInv() {

  Int newValue;
  Int inverse;

  subp[0].Set(&ints[0]);

  // Unroll small loops for better performance
  if (size >= 8) {
    for (int i = 1; i < size; i += 8) {
      if (i < size) subp[i].ModMulK1(&subp[i - 1], &ints[i]);
      if (i+1 < size) subp[i+1].ModMulK1(&subp[i], &ints[i+1]);
      if (i+2 < size) subp[i+2].ModMulK1(&subp[i+1], &ints[i+2]);
      if (i+3 < size) subp[i+3].ModMulK1(&subp[i+2], &ints[i+3]);
      if (i+4 < size) subp[i+4].ModMulK1(&subp[i+3], &ints[i+4]);
      if (i+5 < size) subp[i+5].ModMulK1(&subp[i+4], &ints[i+5]);
      if (i+6 < size) subp[i+6].ModMulK1(&subp[i+5], &ints[i+6]);
      if (i+7 < size) subp[i+7].ModMulK1(&subp[i+6], &ints[i+7]);
    }
  } else {
    for (int i = 1; i < size; i++) {
      subp[i].ModMulK1(&subp[i - 1], &ints[i]);
    }
  }

  // Do the inversion
  inverse.Set(&subp[size - 1]);
  inverse.ModInv();

  // Unroll the back-substitution loop
  if (size >= 8) {
    for (int i = size - 1; i > 0; i -= 8) {
      if (i >= 8) {
        newValue.ModMulK1(&subp[i - 1], &inverse);
        inverse.ModMulK1(&ints[i]);
        ints[i].Set(&newValue);

        newValue.ModMulK1(&subp[i - 2], &inverse);
        inverse.ModMulK1(&ints[i-1]);
        ints[i-1].Set(&newValue);

        newValue.ModMulK1(&subp[i - 3], &inverse);
        inverse.ModMulK1(&ints[i-2]);
        ints[i-2].Set(&newValue);

        newValue.ModMulK1(&subp[i - 4], &inverse);
        inverse.ModMulK1(&ints[i-3]);
        ints[i-3].Set(&newValue);

        newValue.ModMulK1(&subp[i - 5], &inverse);
        inverse.ModMulK1(&ints[i-4]);
        ints[i-4].Set(&newValue);

        newValue.ModMulK1(&subp[i - 6], &inverse);
        inverse.ModMulK1(&ints[i-5]);
        ints[i-5].Set(&newValue);

        newValue.ModMulK1(&subp[i - 7], &inverse);
        inverse.ModMulK1(&ints[i-6]);
        ints[i-6].Set(&newValue);

        newValue.ModMulK1(&subp[i - 8], &inverse);
        inverse.ModMulK1(&ints[i-7]);
        ints[i-7].Set(&newValue);
      }
    }
  } else {
    for (int i = size - 1; i > 0; i--) {
      newValue.ModMulK1(&subp[i - 1], &inverse);
      inverse.ModMulK1(&ints[i]);
      ints[i].Set(&newValue);
    }
  }

  ints[0].Set(&inverse);

}
