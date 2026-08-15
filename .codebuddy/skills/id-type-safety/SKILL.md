---
name: id-type-safety
description: 防止云端雪花 ID（64-bit string）在 C++/Qt 代码中被错误地用 toInt() 转换导致溢出为 0。此 skill 在编写或修改涉及 ID 字段（如 ingrId、emsId、cateId、recoId、productId）的代码时使用，包括 JSON 解析、上传构造、数据库读写、QML 传参等场景。适用于 SmartScale 项目中所有与后端 API 交互的 C++ 服务层和数据模型代码。
---

# ID 类型安全防护

## 概述

SmartScale 后端使用雪花 ID（64-bit long），JSON 中以 **string** 类型返回（如 `"ingrId":"61128584684638212"`）。C++ 中若用 `toInt()`（32-bit）转换，17 位数字会**溢出为 0 或负数**，导致上传数据丢失关联。

此 skill 提供 ID 字段处理规则、审查清单和正确代码模式，在编写/修改涉及 ID 的代码时强制执行类型安全检查。

## 核心规则

### 规则 1：云端雪花 ID 全程保持 QString

后端返回的雪花 ID 字段（ingrId、emsId、cateId、recoId、productId），**解析时一律存为 QString**，禁止转为 int：

```cpp
// ✅ 正确：用 toVariant().toString() 兼容 string/number 两种 JSON 格式
QString ingrId = obj.value("ingrId").toVariant().toString();
QString emsId  = obj.value("emsId").toVariant().toString();
QString cateId = obj.value("cateId").toVariant().toString();

// ❌ 错误：toInt() 溢出
int ingrId = obj.value("ingrId").toInt();
```

### 规则 2：上传时 QString → qint64 → QJsonValue

上传 JSON 构造时，将 QString 转为 `qint64` 再包装为 `QJsonValue(qlonglong)`：

```cpp
// ✅ 正确
bool ok;
qint64 val = record.ingrId.toLongLong(&ok);
json["ingrId"] = ok ? QJsonValue(qlonglong(val)) : 0;

// ❌ 错误：toInt() 溢出
json["ingrId"] = record.ingrId.toInt();

// ❌ 错误：直接传 QString，后端可能拒绝
json["ingrId"] = record.ingrId;
```

### 规则 3：本地 DB 自增 ID 可用 toInt

SQLite 自增主键（weight_records.id、users.id、hardware_config.id）是 32-bit 安全的，`toInt()` 正确：

```cpp
// ✅ 本地 DB 自增 ID，toInt 安全
int dbId = q.value("id").toInt();
```

### 规则 4：响应解析兼容 string/number

后端可能返回 string 或 number 两种格式，解析时必须兜底：

```cpp
// ✅ 兼容两种格式
QJsonValue c = dataObj.value("custId");
int custId = c.isString() ? c.toString().toInt() : c.toInt();

// ❌ 假设后端永远返回 number
int custId = dataObj.value("custId").toInt();
```

## 工作流：编写/修改 ID 相关代码时

### 步骤 1：识别 ID 字段

遇到以下字段时触发类型安全检查：
- **云端雪花 ID**（必须 string / qint64）：`ingrId`、`emsId`、`cateId`、`recoId`、`userId`、`productId`
- **业务小整数**（兜底解析）：`custId`、`devId`、`bill`
- **本地 DB ID**（toInt 安全）：`id`（SQLite 自增）

### 步骤 2：检查转换方式

对每个 ID 字段，确认：
1. **解析**：是否用 `toVariant().toString()` 或 `isString()` 兜底？
2. **存储**：数据模型中是否声明为 `QString` 而非 `int`？
3. **上传**：是否用 `toLongLong()` + `QJsonValue(qlonglong)`？

### 步骤 3：审查 toInt() 调用

在新增/修改的代码中搜索所有 `.toInt()` 调用，逐一确认：
- 如果作用于**云端雪花 ID** → **必须改为** `toLongLong()` 或保持 QString
- 如果作用于**本地 DB 自增 ID** → 安全，保留
- 如果作用于**HTTP 状态码/计数** → 安全，保留

### 步骤 4：参考字段清单

加载 `references/id-fields-inventory.md` 获取完整字段清单、每个字段的来源接口和正确处理代码示例。

## 常见陷阱场景

### 陷阱 1：buildUploadJson 中 toInt

```cpp
// 触发场景：构造上传 JSON
// 危险：ingrId 是 17 位雪花 ID
json["ingrId"] = record.ingrId.toInt();  // → 0，数据丢失

// 修复
bool ok;
qint64 val = record.ingrId.toLongLong(&ok);
json["ingrId"] = ok ? QJsonValue(qlonglong(val)) : 0;
```

### 陷阱 2：响应解析未兜底 string

```cpp
// 触发场景：解析后端响应
// 危险：后端可能返回 "2" 而非 2
int custId = dataObj.value("custId").toInt();  // → 0

// 修复
QJsonValue c = dataObj.value("custId");
int custId = c.isString() ? c.toString().toInt() : c.toInt();
```

### 陷阱 3：getIngrId() 返回值链式 toInt

```cpp
// 触发场景：服务方法返回 QString，调用方误用 toInt
json["ingrId"] = m_ingredientSvc->getIngrId(ingrCd).toInt();  // 溢出

// 修复
bool ok;
qint64 val = m_ingredientSvc->getIngrId(ingrCd).toLongLong(&ok);
json["ingrId"] = ok ? QJsonValue(qlonglong(val)) : 0;
```

## 审查清单（提交前自查）

编写涉及 ID 的代码后，逐项确认：

- [ ] 所有云端雪花 ID（ingrId/emsId/cateId/recoId/productId）解析时用 `toVariant().toString()`
- [ ] 数据模型中云端 ID 字段声明为 `QString`，不是 `int`
- [ ] 上传 JSON 构造时云端 ID 用 `toLongLong()` + `QJsonValue(qlonglong)`
- [ ] 搜索新增代码中所有 `.toInt()`，确认无云端雪花 ID 混入
- [ ] 后端响应解析用 `isString()` 兜底 custId/devId/userId 等

## 参考资料

- `references/id-fields-inventory.md`：项目完整 ID 字段清单，含每个字段的来源接口、正确/错误代码示例
