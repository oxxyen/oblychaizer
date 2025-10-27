// просто каркас!

import { createClient } from 'redis';
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const config = {
  redis: {
    host: process.env.REDIS_HOST || '127.0.0.1',
    port: parseInt(process.env.REDIS_PORT, 10) || 6379,
    password: process.env.REDIS_PASSWORD || undefined,
    db: parseInt(process.env.REDIS_DB, 10) || 0,
  },
  keyPrefix: process.env.REDIS_KEY_PREFIX || 'active_vpn:',
};

/**
 * Валидатор IP-адресов (IPv4/IPv6)
 */
class IPValidator {
  static isValidIPv4(ip) {
    if (typeof ip !== 'string') return false;
    const blocks = ip.split('.');
    if (blocks.length !== 4) return false;
    return blocks.every(block => {
      const num = Number(block);
      return Number.isInteger(num) && num >= 0 && num <= 255 && String(num) === block;
    });
  }

  static isValidIPv6(ip) {
    if (typeof ip !== 'string') return false;
    // Простая проверка IPv6 (для продакшена лучше использовать net.isIPv6)
    return Boolean(ip.includes(':') && ip.split(':').length >= 2);
  }

  static isValid(ip) {
    return this.isValidIPv4(ip) || this.isValidIPv6(ip);
  }
}

/**
 * Клиент Redis с инкапсуляцией и управлением соединением
 */
class RedisClient {
  constructor(config) {
    this.config = config;
    this.client = null;
  }

  async connect() {
    if (this.client?.isOpen) return;
    this.client = createClient({
      socket: {
        host: this.config.host,
        port: this.config.port,
      },
      password: this.config.password,
      database: this.config.db,
    });

    this.client.on('error', (err) => {
      console.error(`[RedisClient] Connection error: ${err.message}`);
    });

    await this.client.connect();
  }

  async disconnect() {
    if (this.client?.isOpen) {
      await this.client.disconnect();
    }
  }

  async exists(key) {
    if (!this.client?.isOpen) throw new Error('Redis client not connected');
    return await this.client.exists(key);
  }
}

/**
 * Сервис проверки активных VPN-соединений по IP
 */
class ActiveVPNChecker {
  constructor(redisClient, keyPrefix = 'active_vpn:') {
    this.redis = redisClient;
    this.keyPrefix = keyPrefix;
  }

  _buildKey(ip) {
    return `${this.keyPrefix}${ip}`;
  }

  async isIPActive(ip) {
    if (!IPValidator.isValid(ip)) {
      throw new Error(`Invalid IP address: ${ip}`);
    }

    const key = this._buildKey(ip);
    try {
      const exists = await this.redis.exists(key);
      return Boolean(exists);
    } catch (err) {
      console.error(`[ActiveVPNChecker] Failed to check IP ${ip}:`, err.message);
      throw new Error(`Redis query failed for IP ${ip}`);
    }
  }
}

/**
 * Основной запуск — CLI или модульный вызов
 */
async function main(ipToCheck) {
  if (!ipToCheck) {
    console.error('Usage: node ip_validator.mjs <IP_ADDRESS>');
    process.exit(1);
  }

  const redis = new RedisClient(config.redis);
  const checker = new ActiveVPNChecker(redis, config.keyPrefix);

  try {
    await redis.connect();
    const isActive = await checker.isIPActive(ipToCheck);
    console.log(JSON.stringify({
      ip: ipToCheck,
      active: isActive,
      timestamp: new Date().toISOString(),
    }, null, 2));
  } catch (err) {
    console.error(`[FATAL] ${err.message}`);
    process.exit(1);
  } finally {
    await redis.disconnect();
  }
}

// Поддержка CLI: node ip_validator.mjs 192.168.1.100
if (typeof process !== 'undefined' && process.argv[1] === import.meta.url.slice(7)) {
  const ipArg = process.argv[2];
  main(ipArg).catch(console.error);
}

// Экспорт для использования в других модулях
export { IPValidator, RedisClient, ActiveVPNChecker };