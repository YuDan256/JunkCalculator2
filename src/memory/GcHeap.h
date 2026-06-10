// GcHeap.h
#ifndef JC2_GCHEAP_H
#define JC2_GCHEAP_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace jc {

    class Value;

    enum class ObjType {
        STRING, BIGINT, FRACTION, COMPLEX, BASENUM,
        REAL_MATRIX, COMPLEX_MATRIX, STRING_MATRIX,
        LIST, DICT, SET,
        CLOSURE, CLASS, INSTANCE, SUPER_PROXY, SYMBOLIC, NAMESPACE
    };

    struct Obj {
        ObjType type;
        bool isMarked;
        uint32_t refCount = 0; // ★ 新增引用计数，用于 COW
        Obj* next;
        virtual ~Obj() = default;
        virtual void clear() {} // ★ 新增：在真正 delete 前清理内部引用的 Value，防止循环引用导致的 Use-After-Free
        virtual void clearTotal() { clear(); } // 供 GC 调用，无视冻结状态
    };

    class GcHeap {
    public:
        static GcHeap& get() {
            static GcHeap instance;
            return instance;
        }

        template<typename T, typename... Args>
        T* allocate(Args&&... args) {
            T* object = new T(std::forward<Args>(args)...);
            object->isMarked = false;
            object->next = objects_;
            objects_ = object;
            allocsSinceGc_++;
            return object;
        }

        bool shouldCollect() const {
            return allocsSinceGc_ >= gcThreshold_;
        }

        int sweep() {
            Obj** object = &objects_;
            int freed = 0;
            std::vector<Obj*> garbage;

            while (*object != nullptr) {
                if (!(*object)->isMarked) {
                    Obj* unreached = *object;
                    *object = unreached->next;
                    garbage.push_back(unreached);
                    freed++;
                } else {
                    (*object)->isMarked = false;
                    object = &(*object)->next;
                }
            }

            // ★ 核心修复：先统一触发 clearTotal() 断开所有 Value 引用，防止 A->B->A 循环引用时，
            // A 被 delete 后，B 的析构函数再去减 A 的 refCount 导致 Use-After-Free 崩溃！
            for (Obj* unreached : garbage) {
                unreached->clearTotal();
            }

            for (Obj* unreached : garbage) {
                delete unreached;
            }

            allocsSinceGc_ = 0;
            gcThreshold_ = std::max(static_cast<size_t>(256), trackedCount() * 2);
            return freed;
        }

        size_t trackedCount() const {
            size_t count = 0;
            Obj* curr = objects_;
            while(curr) { count++; curr = curr->next; }
            return count;
        }
        size_t threshold() const { return gcThreshold_; }
        size_t allocsSinceGc() const { return allocsSinceGc_; }

        void pushTempRoot(Obj* obj) { tempObjRoots_.push_back(obj); }
        void popTempRoot() { tempObjRoots_.pop_back(); }
        const std::vector<Obj*>& getTempObjRoots() const { return tempObjRoots_; }

        void pushTempValueRoot(Value* val) { tempValueRoots_.push_back(val); }
        void popTempValueRoot() { tempValueRoots_.pop_back(); }
        const std::vector<Value*>& getTempValueRoots() const { return tempValueRoots_; }

    private:
        GcHeap() = default;
        std::vector<Obj*> tempObjRoots_;
        std::vector<Value*> tempValueRoots_;
        Obj* objects_ = nullptr;
        size_t allocsSinceGc_ = 0;
        size_t gcThreshold_ = 256;
    };

    struct GcObjGuard {
        GcObjGuard(Obj* obj) { GcHeap::get().pushTempRoot(obj); }
        ~GcObjGuard() { GcHeap::get().popTempRoot(); }
        GcObjGuard(const GcObjGuard&) = delete;
        GcObjGuard& operator=(const GcObjGuard&) = delete;
    };

    struct GcValueGuard {
        GcValueGuard(Value& val) { GcHeap::get().pushTempValueRoot(&val); }
        ~GcValueGuard() { GcHeap::get().popTempValueRoot(); }
        GcValueGuard(const GcValueGuard&) = delete;
        GcValueGuard& operator=(const GcValueGuard&) = delete;
    };

} // namespace jc
#endif // JC2_GCHEAP_H
