#include "WeightSensorWorker.h"
#include <QDebug>
#include <QElapsedTimer>

// ============================================================================
// 构造 / 析构
// ============================================================================

WeightSensorWorker::WeightSensorWorker(QObject *parent)
    : QObject(parent)
    , m_serial(nullptr)
    , m_pollTimer(new QTimer(this))
    , m_portName(QStringLiteral("/dev/ttyAMA0"))
    , m_baudRate(9600)
    , m_slaveAddr(0x01)
    , m_pollIntervalMs(DEFAULT_POLL_INTERVAL_MS)
    , m_readTimeoutMs(DEFAULT_READ_TIMEOUT_MS)
{
    loadConfigFromEnv();
    m_pollTimer->setSingleShot(false);

    if (!initSerial()) {
        qWarning() << "[WeightSensorWorker] 串口初始化失败，将在无数据模式下运行";
    }

    qDebug() << "[WeightSensorWorker] 初始化完成, 串口:"
             << (m_serial ? m_serial->portName() : "(无)")
             << "thread:" << QThread::currentThread();
}

WeightSensorWorker::~WeightSensorWorker()
{
    if (m_serial && m_serial->isOpen()) {
        m_serial->close();
    }
}

// ============================================================================
// 启动轮询
// ============================================================================

void WeightSensorWorker::startPolling()
{
    connect(m_pollTimer, &QTimer::timeout, this, &WeightSensorWorker::poll);
    m_pollTimer->start(m_pollIntervalMs);
    qDebug() << "[WeightSensorWorker] 轮询已启动, interval=" << m_pollIntervalMs << "ms";
}

// ============================================================================
// 环境变量加载 — 启动时一次性读取, 失败时保留默认值
// ============================================================================

void WeightSensorWorker::loadConfigFromEnv()
{
    // 串口设备路径
    QByteArray portEnv = qgetenv("SMARTSCALE_SERIAL_PORT");
    if (!portEnv.isEmpty()) {
        m_portName = QString::fromLocal8Bit(portEnv);
    }

    // 波特率
    QByteArray baudEnv = qgetenv("SMARTSCALE_SERIAL_BAUD");
    bool baudOk = false;
    int baudVal = baudEnv.toInt(&baudOk);
    if (baudOk && baudVal > 0) {
        m_baudRate = baudVal;
    }

    // Modbus 从站地址
    QByteArray slaveEnv = qgetenv("SMARTSCALE_MODBUS_SLAVE");
    bool slaveOk = false;
    int slaveVal = slaveEnv.toInt(&slaveOk);
    if (slaveOk && slaveVal > 0 && slaveVal <= 247) {
        m_slaveAddr = static_cast<uint8_t>(slaveVal);
    }

    // 轮询间隔
    QByteArray pollEnv = qgetenv("SMARTSCALE_POLL_INTERVAL_MS");
    bool pollOk = false;
    int pollVal = pollEnv.toInt(&pollOk);
    if (pollOk && pollVal >= 20 && pollVal <= 5000) {
        m_pollIntervalMs = pollVal;
    }

    // 读响应总超时
    QByteArray timeoutEnv = qgetenv("SMARTSCALE_READ_TIMEOUT_MS");
    bool timeoutOk = false;
    int timeoutVal = timeoutEnv.toInt(&timeoutOk);
    if (timeoutOk && timeoutVal >= 100 && timeoutVal <= 10000) {
        m_readTimeoutMs = timeoutVal;
    }

    qDebug().nospace() << "[WeightSensorWorker] 配置: port=" << m_portName
                       << " baud=" << m_baudRate
                       << " slave=0x" << QString::number(m_slaveAddr, 16)
                       << " poll=" << m_pollIntervalMs << "ms"
                       << " timeout=" << m_readTimeoutMs << "ms"
#ifdef SMARTSCALE_MODBUS_PROTOCOL_V2
                       << " protocol=V2"
#else
                       << " protocol=Feigong"
#endif
                       ;
}

// ============================================================================
// 串口初始化
// ============================================================================

bool WeightSensorWorker::initSerial()
{
    m_serial = new QSerialPort(this);
    m_serial->setPortName(m_portName);
    m_serial->setBaudRate(m_baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        qWarning() << "[WeightSensorWorker] 无法打开串口"
                   << m_portName << "-" << m_serial->errorString();
        delete m_serial;
        m_serial = nullptr;
        return false;
    }

    qDebug() << "[WeightSensorWorker] 串口已打开:" << m_portName
             << "@" << m_baudRate << "8N1 slave=0x"
             << QString::number(m_slaveAddr, 16);
    return true;
}

// ============================================================================
// 定时轮询 — 在 Worker 线程中执行, 阻塞 I/O 不影响 UI
// ============================================================================

void WeightSensorWorker::poll()
{
    // 命令执行中（去皮/校准/读SN）时跳过本轮轮询，避免抢串口
    if (m_isBusy) return;

    int32_t weightG = 0;
    uint16_t statusWord = 0;
    int32_t adcRaw = 0;

    QMutexLocker locker(&m_serialMutex);
    int ret = modbusReadWeight(&weightG, &statusWord, &adcRaw);

    if (ret == 0) {
        m_consecutiveErrors = 0;  // 成功则重置错误计数
        Q_EMIT weightDataReady(weightG, statusWord, adcRaw);
    } else {
        m_consecutiveErrors++;
        if (m_consecutiveErrors >= kMaxConsecutiveErrors) {
            qWarning() << "[WeightSensorWorker] 连续" << m_consecutiveErrors << "次通信失败，尝试重启串口...";
            restartSerial();
        }
    }
}

// ============================================================================
// 去皮 — 在 Worker 线程中执行
// ============================================================================

void WeightSensorWorker::doTare()
{
    if (!m_serial || !m_serial->isOpen()) {
        qWarning() << "[WeightSensorWorker] 去皮失败: 串口未打开";
        Q_EMIT tareDone(false);
        return;
    }

    // 原子抢锁：期望 m_isBusy==false，成功则设为 true 并继续
    // compare_exchange_strong 保证只有一个线程能通过（poll 或其他命令）
    bool expected = false;
    if (!m_isBusy.compare_exchange_strong(expected, true)) {
        qDebug() << "[WeightSensorWorker] 去皮被跳过：前一个命令仍在执行中";
        Q_EMIT tareDone(false);
        return;
    }
    // ★ 此时 m_isBusy 已原子性地设为 true，poll() 必然看到 true 并跳过

    qDebug() << "[WeightSensorWorker] >>> 发送去皮命令...";

    int ret = -1;
    {
        QMutexLocker locker(&m_serialMutex);
        ret = modbusWriteCmd(REG_CMD_ADDR, CMD_TARE);
    }   // ← locker 析构 → 自动 unlock

    if (ret == 0) {
        // 去皮成功后等待秤恢复: 秤执行硬件去皮(清零+稳秤)需要数百ms
        // 若立即恢复轮询, 秤无法响应导致连续无响应→触发串口重启
        QThread::msleep(400);

        // ★ 秤恢复后才释放 busy, 确保 poll() 不会在秤忙期间抢串口
        m_isBusy.store(false, std::memory_order_release);

        m_consecutiveErrors = 0;
        qDebug() << "[WeightSensorWorker] 去皮成功";
        Q_EMIT tareDone(true);
    } else {
        m_isBusy.store(false, std::memory_order_release);
        m_consecutiveErrors++;
        qWarning() << "[WeightSensorWorker] 去皮失败, err=" << ret;
        Q_EMIT tareDone(false);
    }
}

// ============================================================================
// 校准 — 预留接口, 转发到 modbusWriteCmd
//   cmd = CMD_CALIB_HALF(2) 半量程标定
//   cmd = CMD_CALIB_FULL(3) 满量程标定
// 注意: 校准前需先去皮 (空载), 再放砝码, 再发校准命令。
//       V2 旧协议只支持单一校准命令 (CMD_CALIBRATE=2), 此处走 Feigong 定义。
// ============================================================================

void WeightSensorWorker::doCalibrate(uint16_t cmd)
{
    if (!m_serial || !m_serial->isOpen()) {
        qWarning() << "[WeightSensorWorker] 校准失败: 串口未打开";
        Q_EMIT calibrateDone(false);
        return;
    }

    if (cmd != CMD_CALIB_HALF && cmd != CMD_CALIB_FULL) {
        qWarning() << "[WeightSensorWorker] 校准失败: 非法命令值=" << cmd;
        Q_EMIT calibrateDone(false);
        return;
    }

    // 原子抢锁
    bool expected = false;
    if (!m_isBusy.compare_exchange_strong(expected, true)) {
        qDebug() << "[WeightSensorWorker] 校准被跳过：前一个命令仍在执行中";
        Q_EMIT calibrateDone(false);
        return;
    }

    qDebug() << "[WeightSensorWorker] >>> 发送校准命令, cmd=" << cmd;

    int ret = -1;
    {
        QMutexLocker locker(&m_serialMutex);
        ret = modbusWriteCmd(REG_CMD_ADDR, cmd);
    }   // ← locker 析构 → 自动 unlock

    m_isBusy.store(false, std::memory_order_release);

    if (ret == 0) {
        m_consecutiveErrors = 0;
        qDebug() << "[WeightSensorWorker] 校准命令已确认";
        Q_EMIT calibrateDone(true);
    } else {
        m_consecutiveErrors++;
        qWarning() << "[WeightSensorWorker] 校准失败, err=" << ret;
        Q_EMIT calibrateDone(false);
    }
}

// ============================================================================
// 串口自愈 — 连续 kMaxConsecutiveErrors 次通信失败后自动重启
// ============================================================================

void WeightSensorWorker::restartSerial()
{
    qWarning() << "[WeightSensorWorker] 正在重启串口...";

    // 暂停轮询，防止重启期间 poll 抢串口
    m_pollTimer->stop();

    // 加锁保护整个重启过程
    QMutexLocker locker(&m_serialMutex);

    if (m_serial) {
        if (m_serial->isOpen()) {
            m_serial->close();
            qInfo() << "[WeightSensorWorker] 串口已关闭";
        }
        delete m_serial;
        m_serial = nullptr;
    }

    // 等待硬件释放
    QThread::msleep(200);

    // 重新初始化
    if (initSerial()) {
        qInfo() << "[WeightSensorWorker] 串口重启成功:" << m_portName;
        m_consecutiveErrors = 0;   // 重置错误计数
        m_isBusy.store(false, std::memory_order_release);  // 原子清除忙标志
    } else {
        qCritical() << "[WeightSensorWorker] 串口重启失败！将在下次尝试...";
    }

    // 恢复轮询
    m_pollTimer->start(m_pollIntervalMs);
}

// ============================================================================
// Modbus CRC-16
// ============================================================================

uint16_t WeightSensorWorker::crc16Modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if ((crc & 0x0001))
                { crc >>= 1; crc ^= 0xA001; }
            else
                { crc >>= 1; }
        }
    }
    return crc;
}

// ============================================================================
// 字节序转换
// ============================================================================

int32_t WeightSensorWorker::beToInt32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] << 24 |
                     (uint32_t)p[1] << 16 |
                     (uint32_t)p[2] << 8  |
                     (uint32_t)p[3]);
}

uint16_t WeightSensorWorker::beToUint16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

int32_t WeightSensorWorker::leToInt32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] |
                     (uint32_t)p[1] << 8  |
                     (uint32_t)p[2] << 16 |
                     (uint32_t)p[3] << 24);
}

float WeightSensorWorker::leToFloat(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0]  |
                 (uint32_t)p[1] << 8  |
                 (uint32_t)p[2] << 16 |
                 (uint32_t)p[3] << 24;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

int16_t WeightSensorWorker::beToInt16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// ============================================================================
// Modbus 公共辅助 — 消除三个通信函数的重复样板 (组帧/发送/CRC/日志)
// ============================================================================

void WeightSensorWorker::buildReadFrame(uint16_t regAddr, uint16_t count, uint8_t tx[8])
{
    tx[0] = m_slaveAddr;
    tx[1] = FUNC_READ;
    tx[2] = (regAddr >> 8) & 0xFF;
    tx[3] = regAddr & 0xFF;
    tx[4] = (count >> 8) & 0xFF;
    tx[5] = count & 0xFF;
    uint16_t crc = crc16Modbus(tx, 6);
    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;
}

void WeightSensorWorker::buildWriteFrame(uint16_t regAddr, uint16_t value, uint8_t tx[8])
{
    tx[0] = m_slaveAddr;
    tx[1] = FUNC_WRITE;
    tx[2] = (regAddr >> 8) & 0xFF;
    tx[3] = regAddr & 0xFF;
    tx[4] = (value >> 8) & 0xFF;
    tx[5] = value & 0xFF;
    uint16_t crc = crc16Modbus(tx, 6);
    tx[6] = crc & 0xFF;
    tx[7] = (crc >> 8) & 0xFF;
}

bool WeightSensorWorker::sendFrame(const uint8_t *tx, int len)
{
    if (!m_serial || !m_serial->isOpen()) return false;
    m_serial->clear(QSerialPort::Input);
    qint64 written = m_serial->write((const char *)tx, len);
    if (written != len) {
        qWarning() << "[Modbus] 写入失败, expected=" << len << "actual=" << written;
        return false;
    }
    if (!m_serial->flush()) {
        qWarning() << "[Modbus] flush 失败";
        return false;
    }
    return true;
}

QByteArray WeightSensorWorker::readFrame(int expectedLen)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray rxBuf;
    while (rxBuf.size() < expectedLen && timer.elapsed() < m_readTimeoutMs) {
        if (m_serial->waitForReadyRead(SINGLE_WAIT_MS)) {
            rxBuf += m_serial->readAll();
        }
    }
    return rxBuf;
}

QString WeightSensorWorker::toHexString(const uint8_t *p, int n)
{
    QString s;
    s.reserve(n * 3);
    for (int i = 0; i < n; ++i)
        s.append(QString("%1 ").arg(p[i], 2, 16, QChar('0')).toUpper());
    return s.trimmed();
}

int WeightSensorWorker::verifyResponse(const QByteArray &rx, int frameLen)
{
    uint16_t calcCrc = crc16Modbus((const uint8_t *)rx.data(), frameLen - 2);
    uint16_t recvCrc = ((uint8_t)rx[frameLen - 2]) | (((uint8_t)rx[frameLen - 1]) << 8);
    if (calcCrc != recvCrc) {
        qWarning() << "[Modbus] CRC错误: calc=0x" << QString::number(calcCrc, 16)
                   << "recv=0x" << QString::number(recvCrc, 16);
        return -2;
    }
    if ((uint8_t)rx[1] & 0x80) {
        qWarning() << "[Modbus] 异常响应: 错误码=0x"
                   << QString::number((uint8_t)rx[2], 16);
        return -3;
    }
    return 0;
}

// ============================================================================
// V2 旧协议状态位 → Feigong 标准状态位 重映射
//   V2:  Bit0=去皮  Bit1=稳定  Bit2=负重
//   飞功: Bit0=稳定  Bit1=过载  Bit2=负重  Bit3=去皮
//   (V2 无过载位定义, 重映射后过载位恒为 0)
// ============================================================================

uint16_t WeightSensorWorker::remapV2StatusToFeigong(uint16_t v2Status)
{
    uint16_t out = 0;
    if (v2Status & 0x02) out |= 0x01;  // V2 Bit1 稳定 → Feigong Bit0
    // 过载位 V2 无定义, 留 0
    if (v2Status & 0x04) out |= 0x04;  // V2 Bit2 负重 → Feigong Bit2
    if (v2Status & 0x01) out |= 0x08;  // V2 Bit0 去皮 → Feigong Bit3
    return out;
}

// ============================================================================
// 功能码 06: 写单个寄存器 (去皮/校准)
// 返回: 0=OK  -1=写/读失败  -2=CRC错  -3=异常响应
// ============================================================================

int WeightSensorWorker::modbusWriteCmd(uint16_t regAddr, uint16_t value)
{
    if (!m_serial || !m_serial->isOpen()) return -1;

    uint8_t tx[8];
    buildWriteFrame(regAddr, value, tx);

    // 打印 TX
    qDebug().nospace() << "[Modbus] TX WRITE(" << m_portName << " slave=0x"
                       << QString::number(m_slaveAddr, 16) << "): "
                       << toHexString(tx, 8);

    if (!sendFrame(tx, 8)) return -1;



    // 等待确认帧回显 (功能码06的响应 = 请求原样回显, 固定8B)
    if (!m_serial->waitForReadyRead(500)) {
        qWarning() << "[Modbus] 等待确认帧超时";
        return -1;
    }
    QThread::msleep(30);

    QByteArray rx = m_serial->readAll();
    if (rx.size() < 8) {
        QByteArray rxHex;
        for (int i = 0; i < rx.size(); i++) rxHex.append(QString("%1 ").arg((uint8_t)rx[i], 2, 16, QChar('0')).toUpper().toUtf8());
        qWarning() << "[Modbus] 确认帧长度不足:" << rx.size() << "bytes -" << rxHex.trimmed();
        return -1;
    }

    // 打印 RX
    QByteArray rxHex;
    for (int i = 0; i < rx.size(); i++) rxHex.append(QString("%1 ").arg((uint8_t)rx[i], 2, 16, QChar('0')).toUpper().toUtf8());
    qDebug().nospace() << "[Modbus] RX ACK(" << rx.size() << "B): " << rxHex.trimmed();

    int v = verifyResponse(rx, 8);
    if (v != 0) return v;



    qDebug() << "[Modbus] [OK] 从站确认成功";
    return 0;
}

// ============================================================================
// 功能码 03: 读取设备序列号 SN (0x0031 起 8 个寄存器 = 16 字节 ASCII)
//   请求帧: 01 03 00 31 00 08 15 C3
//   响应帧: 01 03 10 [16B ASCII SN] CRC CRC  (21B)
// 返回: 0=OK  -1=通信失败  -2=CRC错  -3=异常响应
// ============================================================================

int WeightSensorWorker::modbusReadSN(QString *sn)
{
    if (!m_serial || !m_serial->isOpen()) return -1;

    uint8_t tx[8];
    buildReadFrame(REG_SN_ADDR, REG_SN_COUNT, tx);

    // 打印 TX
    qDebug().nospace() << "[Modbus] TX READ_SN: " << toHexString(tx, 8);

    if (!sendFrame(tx, 8)) return -1;

    QByteArray rxBuf = readFrame(SN_FRAME_LEN);
    if (rxBuf.isEmpty()) {
        qDebug() << "[Modbus] SN 无响应";
        return -1;
    }
    if (rxBuf.size() < SN_FRAME_LEN) {
        QByteArray rxHex;
        for (int i = 0; i < rxBuf.size(); i++) rxHex.append(QString("%1 ").arg((uint8_t)rxBuf[i], 2, 16, QChar('0')).toUpper().toUtf8());
        qWarning() << "[Modbus] SN 数据不足: 期望" << SN_FRAME_LEN << "B 实际" << rxBuf.size() << "B -" << rxHex.trimmed();
        return -1;
    }
    if (rxBuf.size() > SN_FRAME_LEN) {
        qDebug() << "[Modbus] SN 收到" << rxBuf.size() << "B, 仅使用前" << SN_FRAME_LEN << "B (粘包)";
    }

    int v = verifyResponse(rxBuf, SN_FRAME_LEN);
    if (v != 0) return v;



    // 数据区: rxBuf[3..18] = 16 字节 ASCII (高位在前, 大端寄存器序)
    QByteArray snBytes = rxBuf.mid(3, 16);

    // 异常数据处理：板子未烧录 SN 时寄存器全为 0xFF，解析后是 'ÿ' 乱码
    // 1) 全 0xFF 视为未烧录
    bool allFF = true;
    for (int i = 0; i < snBytes.size(); ++i) {
        if ((uint8_t)snBytes[i] != 0xFF) { allFF = false; break; }
    }
    if (allFF) {
        qWarning() << "[Modbus] SN 未烧录(全 0xFF)，返回空串";
        *sn = QString();
        return 0;
    }

    // 2) 过滤含不可打印字符的非法数据
    QString snStr = QString::fromLatin1(snBytes).trimmed();
    for (int i = 0; i < snStr.size(); ++i) {
        QChar ch = snStr.at(i);
        if (!ch.isPrint()) {
            qWarning() << "[Modbus] SN 含不可打印字符，返回空串: 0x"
                       << QString::number(ch.unicode(), 16);
            *sn = QString();
            return 0;
        }
    }
    *sn = snStr;

    // 打印 RX
    QByteArray rxHex;
    for (int i = 0; i < rxBuf.size(); i++) rxHex.append(QString("%1 ").arg((uint8_t)rxBuf[i], 2, 16, QChar('0')).toUpper().toUtf8());
    qDebug().nospace() << "[Modbus] RX SN(" << rxBuf.size() << "B): " << rxHex.trimmed();

    return 0;
}

// ============================================================================
// 读 SN 槽 — 由 GUI 线程通过 QueuedConnection 触发
// ============================================================================

void WeightSensorWorker::doReadSN()
{
    if (!m_serial || !m_serial->isOpen()) {
        qWarning() << "[WeightSensorWorker] 读 SN 失败: 串口未打开";
        Q_EMIT snReady(QString());
        return;
    }

    // 原子抢锁
    bool expected = false;
    if (!m_isBusy.compare_exchange_strong(expected, true)) {
        qDebug() << "[WeightSensorWorker] 读 SN 被跳过：前一个命令仍在执行中";
        Q_EMIT snReady(QString());
        return;
    }

    qDebug() << "[WeightSensorWorker] >>> 读取设备序列号...";

    QString sn;
    int ret = -1;
    {
        QMutexLocker locker(&m_serialMutex);
        ret = modbusReadSN(&sn);
    }   // ← locker 析构 → 自动 unlock

    m_isBusy.store(false, std::memory_order_release);

    if (ret == 0) {
        m_consecutiveErrors = 0;
        qDebug() << "[WeightSensorWorker] SN 读取成功:" << sn;
        Q_EMIT snReady(sn);
    } else {
        m_consecutiveErrors++;
        qWarning() << "[WeightSensorWorker] SN 读取失败, err=" << ret;
        Q_EMIT snReady(QString());
    }
}

// ============================================================================
// 功能码 03: 读保持寄存器 (净重+状态+ADC)
// 返回: 0=OK  -1=通信失败  -2=CRC错  -3=异常响应
//
// 关键改进: 超时设为 READ_TIMEOUT_MS(1000ms), 因为运行在 Worker 线程中,
//           长时间阻塞不会影响 UI 响应!
// ============================================================================

int WeightSensorWorker::modbusReadWeight(int32_t *weight_g, uint16_t *status, int32_t *adc_raw)
{
    if (!m_serial || !m_serial->isOpen()) return -1;

    uint8_t tx[8];
    buildReadFrame(REG_DATA_ADDR, REG_DATA_COUNT, tx);

    if (!sendFrame(tx, 8)) return -1;

    QByteArray rxBuf = readFrame(FRAME_LEN);
    if (rxBuf.isEmpty()) {
        qDebug() << "[Modbus] 无响应";
        return -1;
    }

    if (rxBuf.size() < FRAME_LEN) {
        QByteArray rxHex;
        for (int i = 0; i < rxBuf.size(); i++) rxHex.append(QString("%1 ").arg((uint8_t)rxBuf[i], 2, 16, QChar('0')).toUpper().toUtf8());
        qWarning() << "[Modbus] 数据不足: 期望" << FRAME_LEN << "B 实际" << rxBuf.size() << "B -" << rxHex.trimmed();
        return -1;
    }
    if (rxBuf.size() > FRAME_LEN) {
        qDebug() << "[Modbus] 收到" << rxBuf.size() << "B, 仅使用前" << FRAME_LEN << "B (粘包)";
    }

    int v = verifyResponse(rxBuf, FRAME_LEN);
    if (v != 0) return v;



    // ==================== 数据区解析 (按协议宏分支) ====================
    // rxBuf 布局: [0]=slave [1]=func [2]=byteCount [3..]=data ... [last-1..last]=CRC
    const uint8_t *d = (const uint8_t *)rxBuf.data() + 3;

#ifdef SMARTSCALE_MODBUS_PROTOCOL_V2
    // V2 旧协议, 数据区 18 字节:
    //   +0  ADC_Value       (int32, 小端)
    //   +4  EmptyLoad_Value (int32, 小端)
    //   +8  SCALE1          (float, 小端 IEEE754)
    //   +12 WEIGHT          (int32, 小端, 单位 g) — 旧代码曾注释为大端, 实际按小端解析
    //   +16 ContAndStatus   (uint16, 大端)
    *adc_raw   = leToInt32(d + 0);
    int32_t emptyLoad = leToInt32(d + 4);   Q_UNUSED(emptyLoad);
    float scale1 = leToFloat(d + 8);         Q_UNUSED(scale1);
    *weight_g = leToInt32(d + 12);
    uint16_t v2Status = beToUint16(d + 16);
    // V2 → Feigong 状态位归一化 (consumeBuffer 统一按 Feigong 位定义读取)
    *status = remapV2StatusToFeigong(v2Status);
#else
    // Feigong 标准协议, 数据区 10 字节:
    //   +0  Net_Weight  (int32, 大端, 单位 g)
    //   +4  Status_Word (uint16, 大端)
    //   +6  ADC_Raw     (int32, 大端)  — 注意 ADC 实际可能 uint32, 这里按 int32 解释
    *weight_g = beToInt32(d + 0);
    *status   = beToUint16(d + 4);
    *adc_raw  = beToInt32(d + 6);
#endif

    return 0;
}
