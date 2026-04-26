// Stub global_callbacks.h — templates de add/remove handler em vectors.
#ifndef GLOBAL_CALLBACKS_H_
#define GLOBAL_CALLBACKS_H_

#ifdef __cplusplus
#include <vector>

template <class C> void addCallbackHandler(std::vector<C>& vec, C instance) {
    for (uint8_t i = 0; i < vec.size(); i++) {
        if (vec[i] == instance) return;
    }
    vec.push_back(instance);
}

template <class C> void removeCallbackHandler(std::vector<C>& vec, C instance) {
    for (auto it = vec.begin(); it != vec.end(); ++it) {
        if (*it == instance) { vec.erase(it); return; }
    }
}
#endif

#endif
