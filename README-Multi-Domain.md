# 🌐 Multi-Domain VPS Setup Guide

This guide shows you how to run multiple applications on your Ubuntu VPS with different domains pointing to different containers using Traefik as a global reverse proxy.

## 🏗️ Architecture Overview

```
Internet
    ↓
Traefik (Global Reverse Proxy)
    ├─ retroos.yourdomain.com → RetroOS App
    ├─ blog.yourdomain.com → WordPress Blog
    ├─ api.yourdomain.com → API Service
    └─ shop.yourdomain.com → E-commerce App
```

**Benefits:**
- ✅ Multiple apps on one server
- ✅ Automatic SSL certificates (Let's Encrypt)
- ✅ Easy domain management
- ✅ Centralized reverse proxy
- ✅ Container isolation

## 🚀 Quick Setup

### 1. First-Time Setup (Deploy Traefik + RetroOS)

```bash
# Clone your RetroOS repository
git clone <your-repo-url>
cd <your-repo-directory>

# Deploy Traefik AND RetroOS in one command
./deploy-domain.sh -d retroos.yourdomain.com -e your-email@example.com --setup-traefik
```

### 2. Add More Applications

```bash
# For each additional app, just deploy without --setup-traefik
./deploy-domain.sh -d blog.yourdomain.com -e your-email@example.com
```

## 📋 Detailed Setup Instructions

### Step 1: Prepare Your Domains

Ensure your domains point to your VPS IP address:

```bash
# DNS A Records needed:
retroos.yourdomain.com    →    YOUR_VPS_IP
blog.yourdomain.com       →    YOUR_VPS_IP
api.yourdomain.com        →    YOUR_VPS_IP
traefik.yourdomain.com    →    YOUR_VPS_IP (optional, for dashboard)
```

### Step 2: Deploy Global Traefik (First Time Only)

```bash
# Update email in traefik-global.yml
vim traefik-global.yml  # Change your-email@example.com

# Deploy Traefik
docker-compose -f traefik-global.yml up -d

# Verify Traefik is running
docker ps | grep traefik
```

### Step 3: Deploy RetroOS with Domain

```bash
# Quick deployment
./deploy-domain.sh -d retroos.yourdomain.com -e your-email@example.com

# Or manual deployment
cp .env.domain .env
# Edit .env with your domain
docker-compose -f docker-compose.domain.yml up -d
```

### Step 4: Verify Deployment

```bash
# Check all containers
docker ps

# Test your domain
curl -I https://retroos.yourdomain.com

# Check Traefik dashboard (if enabled)
open http://YOUR_VPS_IP:8080
```

## 🔧 Adding More Applications

### Example: WordPress Blog

Create `blog-compose.yml`:

```yaml
version: '3.8'

services:
  wordpress:
    image: wordpress:latest
    container_name: blog-wordpress
    restart: unless-stopped
    environment:
      WORDPRESS_DB_HOST: blog-mysql
      WORDPRESS_DB_USER: wordpress
      WORDPRESS_DB_PASSWORD: secure_password
      WORDPRESS_DB_NAME: wordpress
    volumes:
      - wordpress_data:/var/www/html
    networks:
      - blog-internal
      - traefik-global
    labels:
      - traefik.enable=true
      - traefik.docker.network=traefik-global
      - traefik.http.routers.blog.rule=Host(`blog.yourdomain.com`)
      - traefik.http.routers.blog.entrypoints=websecure
      - traefik.http.routers.blog.tls.certresolver=letsencrypt
      - traefik.http.services.blog.loadbalancer.server.port=80

  mysql:
    image: mysql:8.0
    container_name: blog-mysql
    restart: unless-stopped
    environment:
      MYSQL_DATABASE: wordpress
      MYSQL_USER: wordpress
      MYSQL_PASSWORD: secure_password
      MYSQL_ROOT_PASSWORD: root_secure_password
    volumes:
      - mysql_data:/var/lib/mysql
    networks:
      - blog-internal

volumes:
  wordpress_data:
  mysql_data:

networks:
  blog-internal:
    driver: bridge
  traefik-global:
    external: true
```

Deploy:
```bash
docker-compose -f blog-compose.yml up -d
```

### Example: API Service

Create `api-compose.yml`:

```yaml
version: '3.8'

services:
  api:
    image: your-api-image:latest
    container_name: api-service
    restart: unless-stopped
    environment:
      NODE_ENV: production
    networks:
      - traefik-global
    labels:
      - traefik.enable=true
      - traefik.docker.network=traefik-global
      - traefik.http.routers.api.rule=Host(`api.yourdomain.com`)
      - traefik.http.routers.api.entrypoints=websecure
      - traefik.http.routers.api.tls.certresolver=letsencrypt
      - traefik.http.services.api.loadbalancer.server.port=3000

networks:
  traefik-global:
    external: true
```

## 🛠️ Management Commands

### Global Traefik Management

```bash
# View Traefik logs
docker logs traefik-global -f

# Restart Traefik
docker-compose -f traefik-global.yml restart

# Update Traefik
docker-compose -f traefik-global.yml pull
docker-compose -f traefik-global.yml up -d
```

### Individual App Management

```bash
# RetroOS
docker-compose -f docker-compose.domain.yml logs -f
docker-compose -f docker-compose.domain.yml restart

# Other apps
docker-compose -f blog-compose.yml logs -f
docker-compose -f api-compose.yml restart
```

### Monitor All Applications

```bash
# See all running containers
docker ps

# Monitor resource usage
docker stats

# Check Traefik routing
docker logs traefik-global | grep "rule"
```

## 🔒 Security Best Practices

### 1. Secure Traefik Dashboard

```yaml
# In traefik-global.yml, add authentication
labels:
  - traefik.http.middlewares.auth.basicauth.users=admin:$$2y$$10$$HASHED_PASSWORD
  - traefik.http.routers.traefik.middlewares=auth
```

Generate password:
```bash
htpasswd -nb admin your_password
```

### 2. Firewall Configuration

```bash
# Allow only necessary ports
sudo ufw allow 22    # SSH
sudo ufw allow 80    # HTTP
sudo ufw allow 443   # HTTPS
sudo ufw enable
```

### 3. Database Security

- Never expose database ports externally
- Use strong passwords
- Regular backups
- Keep images updated

## 📊 Monitoring & Logging

### Centralized Logging

Add to your docker-compose files:

```yaml
services:
  your-service:
    logging:
      driver: "json-file"
      options:
        max-size: "10m"
        max-file: "3"
```

### Health Checks

```yaml
services:
  your-service:
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:3000/health"]
      interval: 30s
      timeout: 10s
      retries: 3
```

## 🔄 Backup Strategy

### 1. Application Data

```bash
# Backup volumes
docker run --rm -v retroos_mongodb_data:/source -v $(pwd):/backup alpine tar czf /backup/retroos-db-backup.tar.gz -C /source .

# Restore volumes
docker run --rm -v retroos_mongodb_data:/target -v $(pwd):/backup alpine tar xzf /backup/retroos-db-backup.tar.gz -C /target
```

### 2. Configuration Backup

```bash
# Backup all docker-compose files and configs
tar czf vps-config-backup.tar.gz *.yml *.env traefik-data/
```

## 🚨 Troubleshooting

### Domain Not Working

1. **Check DNS**: Ensure domain points to your VPS IP
   ```bash
   dig retroos.yourdomain.com
   ```

2. **Check Traefik**: Verify Traefik can see your service
   ```bash
   docker logs traefik-global | grep retroos
   ```

3. **Check Labels**: Ensure Traefik labels are correct in docker-compose

### SSL Certificate Issues

1. **Check Let's Encrypt Logs**:
   ```bash
   docker logs traefik-global | grep acme
   ```

2. **Verify Domain Ownership**: Ensure domain is accessible from internet

3. **Check Rate Limits**: Let's Encrypt has rate limits

### Service Not Accessible

1. **Check Container Status**:
   ```bash
   docker-compose -f docker-compose.domain.yml ps
   ```

2. **Check Networks**:
   ```bash
   docker network ls
   docker network inspect traefik-global
   ```

3. **Check Logs**:
   ```bash
   docker-compose -f docker-compose.domain.yml logs
   ```

## 📈 Scaling Considerations

### Load Balancing

For high-traffic applications, add multiple instances:

```yaml
services:
  app:
    deploy:
      replicas: 3
    labels:
      - traefik.http.services.app.loadbalancer.sticky=true
```

### Resource Limits

```yaml
services:
  app:
    deploy:
      resources:
        limits:
          memory: 512M
          cpus: '0.5'
        reservations:
          memory: 256M
          cpus: '0.25'
```

## 🎯 Example VPS Configuration

Here's a complete example of a VPS running multiple applications:

```
Your VPS (123.456.789.10)
├── Traefik (Global Proxy) - :80, :443
├── RetroOS - retroos.yourdomain.com
├── WordPress Blog - blog.yourdomain.com
├── API Service - api.yourdomain.com
├── Portfolio Site - portfolio.yourdomain.com
└── Monitoring - monitor.yourdomain.com
```

All with automatic SSL certificates and centralized management!

## 🎉 Summary

With this setup, you can:

- ✅ Run unlimited applications on one VPS
- ✅ Each app gets its own domain
- ✅ Automatic SSL certificates
- ✅ Easy deployment and management
- ✅ Professional-grade reverse proxy
- ✅ Container isolation and security

Your RetroOS is now part of a scalable, professional multi-domain hosting setup!