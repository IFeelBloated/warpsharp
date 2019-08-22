#pragma once
#include <iostream>
#include <string>
#include <array>
#include <type_traits>
#include <algorithm>
#include <utility>
#include <new>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cmath>

#define self(ClassMember, ...) std::decay_t<decltype(__VA_ARGS__)> ClassMember = __VA_ARGS__
#define Begin begin
#define End end

constexpr auto operator""_size(unsigned long long Value) {
	return static_cast<std::size_t>(Value);
}

constexpr auto operator""_ptrdiff(unsigned long long Value) {
	return static_cast<std::ptrdiff_t>(Value);
}

class Range final {
	struct Iterator final {
		self(Cursor, 0_ptrdiff);
		self(Step, 0_ptrdiff);
		Iterator() = default;
		Iterator(std::ptrdiff_t Cursor, std::ptrdiff_t Step) {
			this->Cursor = Cursor;
			this->Step = Step;
		}
		Iterator(Iterator &&) = default;
		Iterator(const Iterator &) = default;
		auto operator=(Iterator &&)->decltype(*this) = default;
		auto operator=(const Iterator &)->decltype(*this) = default;
		~Iterator() = default;
		auto operator*() const {
			return Cursor;
		}
		auto &operator++() {
			Cursor += Step;
			return *this;
		}
		friend auto operator!=(Iterator IteratorA, Iterator IteratorB) {
			if (IteratorA.Step > 0)
				return IteratorA.Cursor < IteratorB.Cursor;
			else if (IteratorA.Step < 0)
				return IteratorA.Cursor > IteratorB.Cursor;
			else
				return IteratorA.Cursor != IteratorB.Cursor;
		}
	};
	self(Startpoint, 0_ptrdiff);
	self(Endpoint, 0_ptrdiff);
	self(Step, 1_ptrdiff);
public:
	Range() = default;

	template<typename T>
	Range(T Endpoint) {
		if (Endpoint < 0)
			Step = -1;
		this->Endpoint = Endpoint;
	}

	template<typename T1, typename T2>
	Range(T1 Startpoint, T2 Endpoint) {
		if (Startpoint > Endpoint)
			Step = -1;
		this->Startpoint = Startpoint;
		this->Endpoint = Endpoint;
	}


	template<typename T1, typename T2, typename T3>
	Range(T1 Startpoint, T2 Endpoint, T3 Step) {
		this->Startpoint = Startpoint;
		this->Endpoint = Endpoint;
		this->Step = Step;
	}
	Range(Range &&) = default;
	Range(const Range &) = default;
	auto operator=(Range &&)->decltype(*this) = default;
	auto operator=(const Range &)->decltype(*this) = default;
	~Range() = default;
	auto Begin() const {
		return Iterator{ Startpoint, Step };
	}
	auto End() const {
		return Iterator{ Endpoint, Step };
	}
};