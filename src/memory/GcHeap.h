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
        CLOSURE, CLASS, INSTANCE, SUPER_PROXY, SYMBOLIC, NAMESPACE,
        UPVALUE // ★ 新增
    };

    struct Obj {
        ObjType type = ObjType::STRING;
        bool isMarked = false;
        uint32_t refCount = 0; // ★ 新增引用计数，用于 COW
        Obj* next = nullptr;
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

        std::function<void()> markCallback;
        std::function<void()> sweepCallback;

        void markObj(Obj* obj);
        void markValue(const Value& val);
        void collectGarbage();

        template<typename T, typename... Args>
        T* allocate(Args&&... args) {
            if (shouldCollect()) {
                collectGarbage();
            }
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
            size_t surviving = 0;
            Obj* garbageList = nullptr;

            while (*object != nullptr) {
                bool isContainer = (*object)->type == ObjType::LIST || (*object)->type == ObjType::DICT || 
                                   (*object)->type == ObjType::SET || (*object)->type == ObjType::CLOSURE || 
                                   (*object)->type == ObjType::CLASS || (*object)->type == ObjType::INSTANCE || 
                                   (*object)->type == ObjType::SUPER_PROXY || (*object)->type == ObjType::NAMESPACE || 
                                   (*object)->type == ObjType::UPVALUE;
                
                // ★ 核心修复：对于非容器类型（如 BigInt, Matrix, Symbolic 等），它们绝不可能产生循环引用。
                // 因此，只要 refCount > 0，就说明 C++ 栈上或某处有 Value 正在持有它，绝对不能回收！
                // 这完美解决了 CAS 引擎中大量局部 Value 变量未被 GcValueGuard 保护导致的崩溃问题。
                bool isAlive = (*object)->isMarked || (!isContainer && (*object)->refCount > 0);

                if (!isAlive) {
                    Obj* unreached = *object;
                    *object = unreached->next;
                    
                    unreached->next = garbageList;
                    garbageList = unreached;
                    
                    freed++;
                } else {
                    (*object)->isMarked = false;
                    object = &(*object)->next;
                    surviving++;
                }
            }

            // ★ 核心修复：先统一触发 clearTotal() 断开所有 Value 引用，防止 A->B->A 循环引用时，
            // A 被 delete 后，B 的析构函数再去减 A 的 refCount 导致 Use-After-Free 崩溃！
            // 优化：只对包含 Value 引用的复杂对象调用 clearTotal，跳过大量基础类型（如 String, Matrix）的虚函数开销
            Obj* curr = garbageList;
            while (curr != nullptr) {
                if (curr->type == ObjType::LIST || curr->type == ObjType::DICT || 
                    curr->type == ObjType::SET || curr->type == ObjType::CLOSURE || 
                    curr->type == ObjType::INSTANCE || curr->type == ObjType::NAMESPACE || 
                    curr->type == ObjType::UPVALUE) {
                    curr->clearTotal();
                }
                curr = curr->next;
            }

            curr = garbageList;
            while (curr != nullptr) {
                Obj* next = curr->next;
                delete curr;
                curr = next;
            }

            allocsSinceGc_ = 0;
            gcThreshold_ = std::max(static_cast<size_t>(65536), surviving * 2);
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
        size_t gcThreshold_ = 65536;
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
