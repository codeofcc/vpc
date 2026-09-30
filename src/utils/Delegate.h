#pragma once
#include<functional>
#include <map>
#include<vector>
#include<mutex>
namespace AC {
	/************************************************************************
	* @Project:  	Common.Delegate
	* @Decription:  委托，这是一个参考duilib和c#实现的委托，主要在事件回调的设计场景中使用。
	* @Verision:  	v1.0
	* @Author:  	Nie_Xin
	* @Create:  	2020-11-27 18:26:30
	* @LastUpdate:  2020-12-29 14:22:24
	************************************************************************
	* Copyright @ 2020. All rights reserved.
	************************************************************************/



	template<class _Mutex, class _Ret, class... _Types>
	class _Delegate_S;

	template<class _Ret, class... _Types>
	using Delegate_S = _Delegate_S<std::mutex, _Ret, _Types...>;




	//template<class _Ret, class... _Types> class Delegate_S;
	/// <summary>
	/// 委托
	///本类的所有方法只能在单线程中使用，包括调用也不能多线程调用
	/// </summary>
	/// <typeparam name="_Ret">委托的返回值</typeparam>
	/// <typeparam name="..._Types">委托的参数</typeparam>
	template<class _Ret, class... _Types>
	class  _Delegate {
	public:
		/// <summary>
		/// 构造函数
		/// </summary>
		_Delegate() : _current(nullptr) {}
		/// <summary>
		/// 复制构造函数
		/// </summary>
		/// <param name="delegate"></param>
		_Delegate(const _Delegate& delegate) {
			auto methodList = delegate._current;
			if (methodList)
			{
				_current = new MethodList;
				_current->Add(*methodList);
			}
			else
				_current = nullptr;
		}
		/// <summary>
		/// 通过函数指针、lambda、std::function、仿函数构造
		/// </summary>
		/// <typeparam name="T">函数指针（默认调用约定_cdecl）、lambda类型、std::function类型、重载了_Ret operator(_Types...)的类型</typeparam>
		/// <param name="method">函数指针、lambda、std::function、仿函数</param>
		template <class T>
		_Delegate(const T& method) : _current(nullptr) {
			SetValueInternal(method);
		}
		/// <summary>
		/// 通过对象的成员方法构造
		/// </summary>
		/// <typeparam name="O">对象类型</typeparam>
		/// <typeparam name="T">对象的成员方法类型</typeparam>
		/// <param name="pObject">对象指针</param>
		/// <param name="pFn">对象成员方法指针</param>
		template <class O, class T>
		_Delegate(O* pObject, _Ret(T::* pFn)(_Types... args)) : _current(new MethodList) {
			_current->Add(Method({ pObject, *((void**)&pFn) , [pObject, pFn](_Types...args) { return (pObject->*pFn)(args...); } }));
		}
		/// <summary>
		///析构函数
		/// </summary>
		virtual	~_Delegate() {
			if (_current)
				delete _current;
			for (auto i : _deleteList)
				delete i;
		}
		/// <summary>
		/// 复制函数
		/// </summary>
		/// <param name="delegate">本类对象</param>
		/// <returns>本对象的引用</returns>
		_Delegate& operator=(const _Delegate& delegate) {
			SetValueInternal(delegate);
			return	*this;
		}
		/// <summary>
		///通过函数指针、lambda、std::function、仿函数赋值
		/// </summary>
		/// <typeparam name="T">函数指针（默认调用约定_cdecl）、lambda类型、std::function类型、重载了_Ret operator(_Types...)的类型</typeparam>
		/// <param name="method">函数指针、lambda、std::function、仿函数</param>
		/// <returns>本对象的引用</returns>
		template <class T>
		_Delegate& operator=(const T& method) {
			SetValueInternal(method);
			return *this;
		}
		/// <summary>
		/// +=函数指针、lambda、std::function、仿函数、本类对象
		/// 其中只有lambda对象、函数指针、装载函数指针或lambda的std::function具有唯一性，可以被-=，其他仿函数无法被-=
		/// </summary>
		/// <typeparam name="T">函数指针（默认调用约定_cdecl）、lambda类型、std::function类型、重载了_Ret operator(_Types...)的类型、本类</typeparam>
		/// <param name="method">函数指针、lambda、std::function、仿函数、本类对象</param>
		/// <returns>本对象的引用</returns>
		template <class T>
		_Delegate& operator+= (const T& method) {
			AddInternal(method);
			return *this;
		}
		/// <summary>
		/// -=函数指针、lambda、std::function、仿函数、本类对象
		/// 其中只有lambda对象、函数指针、装载函数指针或lambda的std::function具有唯一性，可以被-=，其他仿函数无法被-=
		/// 本类对象相减则是两个函数列表相减，具有唯一标识才能被减去，仿函数相减结果不可预料。
		/// </summary>
		/// <typeparam name="T">函数指针（默认调用约定_cdecl）、lambda类型、std::function类型、重载了_Ret operator(_Types...)的类型、本类</typeparam>
		/// <param name="method">函数指针、lambda、std::function、仿函数、本类对象</param>
		/// <returns>本对象的引用</returns>
		template <class T>
		_Delegate& operator-=(const T& method) {
			RemoveInternal(method);
			return *this;
		}
		/// <summary>
		/// 判断方法列表是否为空
		/// </summary>
		/// <returns>列表是否为空</returns>
		explicit operator bool() const noexcept {
			return _current != nullptr;
		}
		/// <summary>
		/// 调用
		/// </summary>
		/// <param name="...args">delegate的参数</param>
		/// <returns>delegate的返回值</returns>
		_Ret operator ()(_Types... args) {
			MethodList* methodList;
			methodList = _current->AddRef();
			Defer defer{ methodList };
			int n = methodList->GetSize() - 1;
			for (int i = 0; i < n; i++)
				(*methodList)[i].method(args...);
			return (*methodList)[n].method(args...);
		}
	protected:
		class Method {
		public:
			void* pObject = nullptr;
			void* pMethodAdress = nullptr;
			std::function<_Ret(_Types... args)> method;
			bool operator ==(const  Method& method) {
				return  pObject == method.pObject && pMethodAdress == method.pMethodAdress;
			}
		};
		class SharedArray {
		public:
			SharedArray(int cap) {
				_capacity = cap;
				_data = (Method*)new char[sizeof(Method) * cap];
			}
			~SharedArray() {
				for (int i = 0; i < _size; i++)
				{
					_data[i].~Method();
				}
				char* pData = (char*)_data;
				delete[] pData;
			}
			void PushBack(const Method& m) {
				new (_data + _size++)Method(m);
			}
			SharedArray& operator=(const SharedArray& array)
			{
				for (int i = 0; i < array._size; i++)
				{
					new (_data + i) Method(array._data[i]);
				}
				_size = array._size;
				return *this;
			}
			void PushBack(const SharedArray& array, int start, int len) {
				int n = start + len;
				Method* begin = _data + _size - start;
				for (int i = start; i < n; i++)
				{
					new (begin + i) Method(array._data[i]);
				}
				_size += len;
			}
			Method& operator[](int i)
			{
				return _data[i];
			}
			int GetSize() {
				return _size;
			}
			int GetCapacity() {
				return _capacity;
			}
			SharedArray* AddRef() {
				_refCount++;
				return this;
			}
			void Release() {
				_refCount--;
				if (!_refCount)
					delete this;
			}
		private:
			Method* _data;
			int _size = 0;
			int _capacity;
			int _refCount = 1;
		};
		class MethodList {
		public:
			MethodList() :_array(new SharedArray(4)) {}
			MethodList(const MethodList& methodList) {
				_array = methodList._array->AddRef();
				_size = methodList._size;
			}
			~MethodList() {
				_array->Release();
			}
			void Add(const Method& method) {
				if (_size < _array->GetSize())
				{
					auto temp = new SharedArray(_size * 2);
					temp->PushBack(*_array, 0, _size);
					_array->Release();
					_array = temp;
				}
				else if (_array->GetSize() == _array->GetCapacity())
				{
					auto temp = new SharedArray(_array->GetCapacity() * 2);
					*temp = *_array;
					_array->Release();
					_array = temp;
				}
				_array->PushBack(method);
				_size++;
			}
			void Add(const MethodList& methodList) {
				int nSize = _size + methodList._size;
				if (_size < _array->GetSize())
				{
					auto temp = new SharedArray(nSize * 2);
					temp->PushBack(*_array, 0, _size);
					_array->Release();
					_array = temp;
				}
				else if (nSize >= _array->GetCapacity() - 1)
				{
					auto temp = new SharedArray(nSize * 2);
					*temp = *_array;
					_array->Release();
					_array = temp;
				}
				_array->PushBack(*(methodList._array), 0, methodList._size);
				_size = nSize;
			}
			MethodList* Remove(void* pObject, void* pMethodAdress)
			{
				int count = _size - 1;
				if ((*_array)[count].pObject == pObject && (*_array)[count].pMethodAdress == pMethodAdress)
				{
					if (count == 0)
						return nullptr;
					MethodList* methodList = new MethodList(*this);
					methodList->_size--;
					return methodList;
				}
				for (int i = count - 1; i > -1; i--)
				{
					if ((*_array)[i].pObject == pObject && (*_array)[i].pMethodAdress == pMethodAdress)
					{
						MethodList* methodList = new MethodList(*this);
						auto temp = new SharedArray(count * 2);
						temp->PushBack(*_array, 0, i);
						temp->PushBack(*_array, i + 1, _size - i - 1);
						methodList->_array->Release();
						methodList->_array = temp;
						methodList->_size--;
						return methodList;
					}
				}
				return this;
			}
			MethodList* Remove(const MethodList& methodList) {
				if (methodList._size == 1)
				{
					return	Remove(methodList[0].pObject, methodList[0].pMethodAdress);
				}
				std::map<void*, std::map<void*, int>>  searchMap;
				std::vector<int> leftIndexes;
				for (int i = 0; i < methodList._size; i++)
				{
					auto& interMap = searchMap[methodList[i].pObject];
					auto iter = interMap.find(methodList[i].pMethodAdress);
					if (iter != interMap.end())
					{
						iter->second = iter->second + 1;
					}
					else
					{
						interMap[methodList[i].pMethodAdress] = 1;
					}
				}
				for (int i = _size - 1; i > -1; i--)
				{
					auto iter = searchMap.find((*_array)[i].pObject);
					if (iter != searchMap.end())
					{
						auto& interMap = iter->second;
						auto jter = interMap.find((*_array)[i].pMethodAdress);
						if (jter != interMap.end())
						{
							jter->second = jter->second - 1;
							if (jter->second > -1)
							{

							}
							else
							{
								interMap.erase(jter);
								leftIndexes.push_back(i);
							}
						}
						else
						{
							leftIndexes.push_back(i);
						}
					}
					else
					{
						leftIndexes.push_back(i);
					}
				}
				if (leftIndexes.size() == _size)
				{
					return this;
				}
				else if (leftIndexes.size() < 1)
				{
					return nullptr;
				}
				else
				{
					MethodList* newMethodList = new MethodList(*this);
					SharedArray* newArray = new SharedArray(leftIndexes.size());
					newMethodList->_size = leftIndexes.size();
					newMethodList->_array->Release();
					newMethodList->_array = newArray;
					for (int i = leftIndexes.size() - 1; i > -1; i--)
					{
						newArray->PushBack((*_array)[leftIndexes[i]]);
					}
					return newMethodList;
				}
				return 	this;
			}
			Method& operator[](int i) const {
				return (*_array)[i];
			}
			int GetSize() {
				return _size;
			}
			MethodList* AddRef() {
				_refCount++;
				return this;
			}
			void RemoveRef() {
				_refCount--;
			}
			int GetRefCount() {
				return _refCount;
			}
		private:
			SharedArray* _array;
			int _size = 0;
			int _refCount = 1;
		};
		class Defer {
		public:
			MethodList* _methodList;
			~Defer() {
				_methodList->RemoveRef();
			}
		};

		template <class T>
		void SetValueInternal(const T& method) {
			std::function< _Ret(_Types... args)> function = method;
			auto methodList = new MethodList();
			methodList->Add(Method({ (void*)function.target_type().hash_code(),nullptr ,function }));
			SetMethodList(methodList);
		}
		void SetValueInternal(const std::function< _Ret(_Types... args)>& function) {
			auto methodList = new MethodList();
			auto target = function.template target<_Ret(*)(_Types... args)>();
			if (target)
			{
				methodList->Add(Method({ nullptr,(void*)*target ,function }));
			}
			else
			{
				methodList->Add(Method({ (void*)function.target_type().hash_code(),nullptr ,function }));
			}
			SetMethodList(methodList);
		}
		void SetValueInternal(_Ret(*method)(_Types... args)) {
			auto methodList = new MethodList();
			methodList->Add({ nullptr,(void*)method ,std::function< _Ret(_Types... args)>(method) });
			SetMethodList(methodList);
		}
		void SetValueInternal(const _Delegate& delegate) {
			if (this == &delegate)
				return;
			auto methodList = delegate._current;
			if (methodList)
			{
				auto methodListNew = new MethodList();
				methodListNew->Add(*methodList);
				SetMethodList(methodListNew);
			}
			else
			{
				SetMethodList(nullptr);
			}
		}
		void SetValueInternal(const Delegate_S<_Ret, _Types...>& delegate) {
			if (this == &delegate)
				return;
			auto methodList = delegate._current;
			if (methodList)
			{
				auto methodListNew = new MethodList();
				methodListNew->Add(*methodList);
				SetMethodList(methodListNew);
			}
			else
			{
				SetMethodList(nullptr);
			}
		}
		template <class T>
		void AddInternal(const T& method) {
			std::function< _Ret(_Types... args)> function = method;
			auto methodList = _current ? new MethodList(*_current) : new MethodList();
			methodList->Add(Method({ (void*)function.target_type().hash_code(),nullptr ,function }));
			SetMethodList(methodList);
		}
		void AddInternal(const std::function< _Ret(_Types... args)>& function) {
			auto methodList = _current ? new MethodList(*_current) : new MethodList();
			auto target = function.template target< _Ret(*)(_Types... args)>();
			if (target)
			{
				methodList->Add(Method({ nullptr,(void*)*target ,function }));
			}
			else
			{
				methodList->Add(Method({ (void*)function.target_type().hash_code(),nullptr ,function }));
			}
			SetMethodList(methodList);
		}
		void AddInternal(_Ret(*method)(_Types... args)) {
			auto methodList = _current ? new MethodList(*_current) : new MethodList();
			methodList->Add(Method({ nullptr,(void*)method ,std::function< _Ret(_Types... args)>(method) }));
			SetMethodList(methodList);
		}
		void AddInternal(const _Delegate& delegate) {
			auto methodList = delegate._current;
			if (methodList)
			{
				auto newMethodList = _current ? new MethodList(*_current) : new MethodList();
				newMethodList->Add(*methodList);
				SetMethodList(newMethodList);
			}
		}
		void AddInternal(const Delegate_S<_Ret, _Types...>& delegate) {
			auto methodList = delegate._current;
			if (methodList)
			{
				auto newMethodList = _current ? new MethodList(*_current) : new MethodList();
				newMethodList->Add(*methodList);
				SetMethodList(newMethodList);
			}
		}
		void RemoveInternal(_Ret(*method)(_Types... args)) {
			if (_current)
			{
				SetMethodList(_current->Remove(nullptr, (void*)method));
			}
		}
		template <class T>
		void RemoveInternal(const T& method) {
			if (_current)
			{
				std::function< _Ret(_Types... args)> function = method;
				SetMethodList(_current->Remove((void*)function.target_type().hash_code(), nullptr));
			}
		}
		void RemoveInternal(const std::function< _Ret(_Types... args)>& function) {
			if (_current)
			{
				auto target = function.template target< _Ret(*)(_Types... args)>();
				if (target)
					SetMethodList(_current->Remove(nullptr, (void*)*target));
				else
					SetMethodList(_current->Remove((void*)function.target_type().hash_code(), nullptr));
			}
		}
		void RemoveInternal(const _Delegate& delegate) {
			if (_current)
			{
				if (this == &delegate)
				{
					SetMethodList(nullptr);
				}
				else
				{
					auto methodList = delegate._current;
					if (methodList && _current)
					{
						SetMethodList(_current->Remove(*methodList));
					}
				}
			}
		}
		void RemoveInternal(const Delegate_S<_Ret, _Types...>& delegate) {
			if (_current)
			{
				if (this == &delegate)
				{
					SetMethodList(nullptr);
				}
				else
				{
					auto methodList = delegate._current;
					if (methodList && _current)
					{
						SetMethodList(_current->Remove(*methodList));
					}
				}
			}
		}
		virtual	void SetMethodList(MethodList* methodList) {
			if (_current && methodList != _current)
			{
				if (_current->GetRefCount() == 1)
					delete _current;
				else
					_deleteList.push_back(_current);
			}
			_current = methodList;
			for (int i = 0; i < _deleteList.size(); i++)
			{
				if (_deleteList[i]->GetRefCount() == 1)
				{
					delete _deleteList[i];
					_deleteList.erase(_deleteList.begin() + i);
					i--;
				}
			}
		}
		MethodList* _current;
		std::vector<MethodList*> _deleteList;
	};


	// ============================================================================
	// 1. 无锁策略 (No-op Mutex)
	// ============================================================================
	struct NullMutex {
		void lock() noexcept {}
		void unlock() noexcept {}
		bool try_lock() noexcept { return true; }
	};

	// ============================================================================
	// 2. _Delegate_S : Mutex 在最前，后接返回值和参数包
	// ============================================================================
	template<class _Mutex, class _Ret, class... _Types>
	class _Delegate_S : public _Delegate<_Ret, _Types...> {
	public:
		using MutexType = _Mutex;

		_Delegate_S() {}

		_Delegate_S(const _Delegate_S& delegate)
			: _Delegate<_Ret, _Types...>(delegate) {}

		template <class T>
		_Delegate_S(const T& method)
			: _Delegate<_Ret, _Types...>(method) {}

		template <class O, class T>
		_Delegate_S(O* pObject, _Ret(T::* pFn)(_Types... args))
			: _Delegate<_Ret, _Types...>(pObject, pFn) {}

		~_Delegate_S() {}

		_Delegate_S& operator=(const _Delegate_S& delegate) {
			LockInternal();
			_Delegate<_Ret, _Types...>::operator=(delegate);
			UnlockInternal();
			return *this;
		}

		template <class T>
		_Delegate_S& operator=(const T& method) {
			LockInternal();
			_Delegate<_Ret, _Types...>::operator=(method);
			UnlockInternal();
			return *this;
		}

		template <class T>
		_Delegate_S& operator+=(const T& method) {
			LockInternal();
			_Delegate<_Ret, _Types...>::operator+=(method);
			UnlockInternal();
			return *this;
		}

		template <class T>
		_Delegate_S& operator-=(const T& method) {
			LockInternal();
			_Delegate<_Ret, _Types...>::operator-=(method);
			UnlockInternal();
			return *this;
		}

		_Ret operator()(_Types... args) {
			auto methodList = this->_current;
			int n = methodList->GetSize() - 1;
			for (int i = 0; i < n; i++)
				(*methodList)[i].method(args...);
			return (*methodList)[n].method(args...);
		}

		void TrimExcess() {
			LockInternal();
			for (auto i : this->_deleteList)
				delete i;
			this->_deleteList.clear();
			UnlockInternal();
		}

	private:
		void SetMethodList(typename _Delegate<_Ret, _Types...>::MethodList* methodList) override {
			if (this->_current && methodList != this->_current) {
				this->_deleteList.push_back(this->_current);
			}
			this->_current = methodList;
		}

		void LockInternal() { _mutex.lock(); }
		void UnlockInternal() { _mutex.unlock(); }

		_Mutex _mutex;
	};

	/// <summary>
/// 成员方法构造delegate
/// </summary>
/// <typeparam name="O">对象类型</typeparam>
/// <typeparam name="T">对象成员方法对应类的类型</typeparam>
/// <typeparam name="_Ret">delegate的返回类型</typeparam>
/// <typeparam name="..._Types">delegate的参数类型<</typeparam>
/// <param name="pObject">对象指针</param>
/// <param name="pFn">成员方法指针</param>
/// <returns>delegate对象</returns>
	template <class O, class T, class _Ret, class... _Types>
	_Delegate<_Ret, _Types...> MakeDelegate(O* pObject, _Ret(T::* pFn)(_Types... args)) {
		return _Delegate<_Ret, _Types...>(pObject, pFn);
	}
	void DelegateTest();



	// ============================================================================
	// 3. Signature Traits
	// ============================================================================
	//template<class>
	//struct DelegateSignature;

	//template<class _Ret, class... _Types>
	//struct DelegateSignature<_Ret(_Types...)> {
	//	using Type = _Delegate<_Ret, _Types...>;
	//};

	/// @brief 内部 trait：Mutex 在最前，用于 _Delegate_S 的类型推导
	template<class _Mutex, class>
	struct _DelegateSignature_S;

	template<class _Mutex, class _Ret, class... _Types>
	struct _DelegateSignature_S<_Mutex, _Ret(_Types...)> {
		using Type = _Delegate_S<_Mutex, _Ret, _Types...>;
	};

	//// ============================================================================
	//// 4. 对外别名
	//// ============================================================================
	///// @brief std::function 风格的委托别名
	//template<class Signature>
	//using DelegateFunc = typename DelegateSignature<Signature>::Type;

	/// @brief std::function 风格的线程安全委托别名
	/// @tparam Signature 函数签名，如 void(int, float)
	/// @tparam _Mutex 互斥锁类型，默认为 NullMutex(无锁)
	/// 
	/// 用法:
	///   DelegateFunc_S<void(int)>                  d1; // 无锁
	///   DelegateFunc_S<void(int), std::mutex>       d2; // std::mutex
	///   DelegateFunc_S<void(int), std::shared_mutex> d3; // shared_mutex
	template<class Signature, class _Mutex = NullMutex>
	using Delegate = typename _DelegateSignature_S<_Mutex, Signature>::Type;

	// ============================================================================
	//// 4. 便捷别名改造
	//// ============================================================================
	///// @brief std::function 风格的委托别名
	//template<class Signature>
	//using DelegateFunc = typename DelegateSignature<Signature>::Type;

}