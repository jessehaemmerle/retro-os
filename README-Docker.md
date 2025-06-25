# RetroOS Docker Deployment Guide

This guide explains how to deploy RetroOS using Docker and Docker Compose.

## Prerequisites

- Docker 20.0+ installed
- Docker Compose 2.0+ installed
- At least 2GB RAM available
- Ports 5050, 5051, 8001, 3000, 27017 available

## Quick Start

### Production Deployment

```bash
# Make the deploy script executable
chmod +x deploy-docker.sh

# Deploy in production mode
./deploy-docker.sh

# Or with options
./deploy-docker.sh -e prod -d -b
```

### Development Deployment

```bash
# Deploy in development mode with hot reload
./deploy-docker.sh -e dev
```

## Manual Deployment

### Production Mode

```bash
# Build and start all services
docker compose -f docker-compose.prod.yml up -d

# View logs
docker compose -f docker-compose.prod.yml logs -f

# Stop services
docker compose -f docker-compose.prod.yml down
```

### Development Mode

```bash
# Build and start all services
docker compose -f docker-compose.yml up -d

# View logs
docker compose -f docker-compose.yml logs -f

# Stop services
docker compose -f docker-compose.yml down
```

## Services Overview

### Architecture

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│    Nginx    │    │  Frontend   │    │   Backend   │    │  MongoDB    │
│   (Proxy)   │◄──►│  (React)    │◄──►│  (FastAPI)  │◄──►│ (Database)  │
│   Port 80   │    │  Port 3000  │    │  Port 8001  │    │ Port 27017  │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘
```

### Service Details

1. **Nginx Proxy** (`nginx`)
   - Serves as reverse proxy
   - Routes `/api/*` to backend
   - Routes everything else to frontend
   - Handles SSL termination (if configured)
   - **Accessible on port 5050**

2. **Frontend** (`frontend`)
   - React application
   - Retro desktop interface
   - Built with Tailwind CSS
   - Connects to backend via `/api` endpoints

3. **Backend** (`backend`)
   - FastAPI application
   - RESTful API for file system, authentication
   - Data persistence via MongoDB
   - Comprehensive error handling

4. **MongoDB** (`mongodb`)
   - Document database
   - Stores user data, files, settings
   - Persistent volume for data storage

## Configuration

### Environment Variables

#### Frontend (.env)
```env
REACT_APP_BACKEND_URL=http://localhost:5050/api
```

#### Backend (.env)
```env
MONGO_URL=mongodb://mongodb:27017
DB_NAME=retroos
```

### Customization

#### Backend URL
To change the backend URL for frontend:

```bash
# In docker-compose.prod.yml
services:
  frontend:
    build:
      args:
        REACT_APP_BACKEND_URL: https://your-domain.com:5050/api
```

#### Database Configuration
To use external MongoDB:

```bash
# In docker-compose.prod.yml
services:
  backend:
    environment:
      MONGO_URL: mongodb://your-mongo-host:27017
      DB_NAME: your_database_name
```

## Monitoring and Debugging

### Health Checks

All services include health checks:

```bash
# Check service health
docker compose -f docker-compose.prod.yml ps

# View specific service health
docker inspect retroos-backend-prod | grep -A 10 Health
```

### Logs

```bash
# View all logs
docker compose -f docker-compose.prod.yml logs -f

# View specific service logs
docker compose -f docker-compose.prod.yml logs -f backend
docker compose -f docker-compose.prod.yml logs -f frontend
docker compose -f docker-compose.prod.yml logs -f mongodb
docker compose -f docker-compose.prod.yml logs -f nginx
```

### Debugging

```bash
# Execute commands in running containers
docker exec -it retroos-backend-prod bash
docker exec -it retroos-frontend-prod sh
docker exec -it retroos-mongodb-prod mongosh

# View container resources
docker stats
```

## Troubleshooting

### Common Issues

1. **Port Already in Use**
   ```bash
   # Check what's using the port
   sudo lsof -i :80
   
   # Stop conflicting services
   sudo systemctl stop apache2 nginx
   ```

2. **Database Connection Issues**
   ```bash
   # Check MongoDB is running
   docker compose -f docker-compose.prod.yml exec mongodb mongosh --eval "db.adminCommand('ping')"
   
   # View backend logs for connection errors
   docker compose -f docker-compose.prod.yml logs backend
   ```

3. **Build Failures**
   ```bash
   # Clean Docker cache
   docker system prune -a
   
   # Rebuild without cache
   docker compose -f docker-compose.prod.yml build --no-cache
   ```

4. **Frontend Not Loading**
   ```bash
   # Check if backend is accessible
   curl http://localhost/api/
   
   # Check nginx configuration
   docker compose -f docker-compose.prod.yml exec nginx nginx -t
   ```

### Performance Optimization

1. **Resource Limits**
   ```yaml
   services:
     backend:
       deploy:
         resources:
           limits:
             memory: 512M
             cpus: '0.5'
   ```

2. **Volume Optimization**
   ```yaml
   volumes:
     mongodb_data:
       driver: local
       driver_opts:
         type: none
         o: bind
         device: /data/mongodb
   ```

## Production Considerations

### Security

1. **Use HTTPS**
   - Configure SSL certificates
   - Update nginx configuration
   - Use proper security headers

2. **Environment Variables**
   - Use Docker secrets for sensitive data
   - Don't hardcode credentials

3. **Network Security**
   - Use custom networks
   - Limit exposed ports
   - Enable firewall rules

### Scaling

1. **Horizontal Scaling**
   ```yaml
   services:
     backend:
       deploy:
         replicas: 3
   ```

2. **Load Balancing**
   - Configure nginx upstream
   - Use external load balancer

### Backup

1. **Database Backup**
   ```bash
   # Create backup
   docker exec retroos-mongodb-prod mongodump --out /backup
   
   # Restore backup
   docker exec retroos-mongodb-prod mongorestore /backup
   ```

2. **Volume Backup**
   ```bash
   # Backup volume
   docker run --rm -v retroos_mongodb_data:/data -v $(pwd):/backup alpine tar czf /backup/mongodb_backup.tar.gz -C /data .
   ```

## Deployment Checklist

- [ ] Docker and Docker Compose installed
- [ ] Required ports available
- [ ] Environment variables configured
- [ ] SSL certificates ready (for HTTPS)
- [ ] Firewall rules configured
- [ ] Backup strategy in place
- [ ] Monitoring configured
- [ ] Health checks working
- [ ] Load testing completed

## Support

For deployment issues:
1. Check logs for error messages
2. Verify all services are healthy
3. Test individual service endpoints
4. Review Docker and system resources
5. Consult Docker documentation

## Updates

To update RetroOS:

```bash
# Pull latest images
docker compose -f docker-compose.prod.yml pull

# Recreate containers
docker compose -f docker-compose.prod.yml up -d

# Clean up old images
docker image prune -f
```