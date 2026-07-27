/*
* kludf.hpp
* KnotLink SDK - C++
* Copyright (c) 2024-2026 KnotLink Contributors
* SPDX-License-Identifier: MIT
*/

#ifndef KLUDF_HPP
#define KLUDF_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace knotlink {
	
	/* ============================================================
	* KLUDF - 通用数据格式基类（预留扩展）
	* ============================================================ */
	
	class KLUDF {
	public:
		virtual ~KLUDF() = default;
		// 预留扩展接口
	};
	
	/* ============================================================
	* KLKVMap - 键值对映射（继承自 unordered_map）
	* ============================================================ */
	
	class KLKVMap : public std::unordered_map<std::string, std::string> {
	public:
		/**
		 * 序列化为键值对字符串
		 * 格式: key1=value1;key2=value2;key3=value3
		 */
		std::string serialize() const {
			std::string result;
			bool first = true;
			for (const auto& pair : *this) {
				if (!first) result += ";";
				result += pair.first + "=" + pair.second;
				first = false;
			}
			return result;
		}
		
		/**
		 * 从键值对字符串反序列化
		 * @param kvString 格式: key1=value1;key2=value2;key3=value3
		 */
		void deserialize(const std::string& kvString) {
			clear();
			if (kvString.empty()) return;
			
			std::stringstream ss(kvString);
			std::string pair;
			while (std::getline(ss, pair, ';')) {
				if (pair.empty()) continue;
				size_t eqPos = pair.find('=');
				if (eqPos != std::string::npos) {
					std::string key = pair.substr(0, eqPos);
					std::string value = pair.substr(eqPos + 1);
					(*this)[key] = value;
				}
			}
		}
		
		/**
		 * 安全读取键值对
		 * @param key 键名
		 * @param defaultVal 键不存在时返回的默认值
		 * @return 键对应的值，或默认值
		 */
		std::string get(const std::string& key, const std::string& defaultVal = "") const {
			auto it = find(key);
			if (it != end()) {
				return it->second;
			}
			return defaultVal;
		}
	};
	
	/* ============================================================
	* 测试代码
	* ============================================================ */
	
#ifdef KLUDF_TEST
#include <iostream>
	
	void test_kludf() {
		// 测试序列化
		knotlink::KLKVMap kvMap;
		kvMap["key1"] = "value1";
		kvMap["key2"] = "value2";
		kvMap["key3"] = "value3";
		
		std::cout << "序列化前: { ";
		for (const auto& pair : kvMap) {
			std::cout << pair.first << "=" << pair.second << " ";
		}
		std::cout << "}" << std::endl;
		
		std::string serialized = kvMap.serialize();
		std::cout << "序列化后: " << serialized << std::endl;
		
		// 测试反序列化
		knotlink::KLKVMap newKvMap;
		newKvMap.deserialize(serialized);
		std::cout << "反序列化后: { ";
		for (const auto& pair : newKvMap) {
			std::cout << pair.first << "=" << pair.second << " ";
		}
		std::cout << "}" << std::endl;
		
		// 测试 get
		std::cout << "获取 key1: " << newKvMap.get("key1") << std::endl;
		std::cout << "获取不存在的 key4: " << newKvMap.get("key4") << std::endl;
		std::cout << "获取不存在的 key4 (默认值): " << newKvMap.get("key4", "default") << std::endl;
	}
	
#endif // KLUDF_TEST
	
} // namespace knotlink

#endif // KLUDF_HPP
