//
// Created by kalmbacj on 3/11/25.
//

#ifndef JOKA_EXAMPLES_H
#define JOKA_EXAMPLES_H
struct C {
    int x;
    bool operator==(const C& otherRhs) const {
    if (x != otherRhs.x) return false;
    if (f != otherRhs.f) return false;
    if (d != otherRhs.d) return false;
    if (arr != otherRhs.arr) return false;
    return true;
  };
    bool f;
    static int s;
    unsigned d = 3;
    int arr[3][4];
};

#endif //JOKA_EXAMPLES_H
