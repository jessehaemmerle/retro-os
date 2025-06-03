# 🎮 RetroOS - Docker Deployment

Welcome to RetroOS! This is a nostalgic Windows 95/98-style web-based operating system built with React, FastAPI, and MongoDB. This guide will help you deploy it easily on your Ubuntu VPS using Docker.

## 🚀 Quick Start

### One-Command Deployment
```bash
git clone <your-repository-url>
cd <repository-directory>
./deploy.sh
```

That's it! Your RetroOS will be running at `http://localhost` (or your server's IP).

## 📋 What You Get

- **🖥️ Retro Desktop Environment**: Complete Windows 95/98 style interface
- **👤 User Authentication**: Secure login system with personalized desktops
- **📁 File System**: Virtual file system with folders, file operations
- **🧮 Calculator**: Functional calculator app
- **📝 Text Editor**: Simple text editing application
- **🌐 Web Browser**: Basic web browsing functionality
- **🎮 Games Launcher**: Access to mini-games
- **🔄 Real-time Updates**: Live desktop state management

## 🐳 Docker Architecture

The application runs in 4 containers:

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│    Nginx    │    │  Frontend   │    │   Backend   │    │  MongoDB    │
│   (Port 80) │ ── │ (Port 3000) │ ── │ (Port 8001) │ ── │ (Port 27017)│
│ Reverse     │    │   React     │    │   FastAPI   │    │  Database   │
│ Proxy       │    │    App      │    │    API      │    │   Storage   │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘
```

## 🛠️ Prerequisites

### Install Docker & Docker Compose on Ubuntu

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install Docker
curl -fsSL https://get.docker.com -o get-docker.sh
sudo sh get-docker.sh

# Add your user to docker group
sudo usermod -aG docker $USER

# Install Docker Compose
sudo curl -L "https://github.com/docker/compose/releases/latest/download/docker-compose-$(uname -s)-$(uname -m)" -o /usr/local/bin/docker-compose
sudo chmod +x /usr/local/bin/docker-compose

# Restart to apply group changes
sudo reboot
```

## 📥 Deployment Options

### Development Mode (Default)
```bash
./deploy.sh
```
- Hot reload enabled
- Development optimizations
- Direct container access

### Production Mode
```bash
./deploy.sh --production
```
- Optimized builds
- Better performance
- Production-ready configuration

## 🔧 Management Commands

### Start/Stop Services
```bash
# Start all services
docker-compose up -d

# Stop all services
docker-compose down

# Restart services
docker-compose restart

# View logs
docker-compose logs -f

# View specific service logs
docker-compose logs -f backend
docker-compose logs -f frontend
```

### Rebuild After Changes
```bash
# Rebuild all services
docker-compose up --build -d

# Rebuild specific service
docker-compose up --build -d backend
```

## 🌐 Access Points

Once deployed, access your RetroOS at:

- **Main Application**: `http://your-server-ip/`
- **Direct Frontend**: `http://your-server-ip:3000`
- **API Documentation**: `http://your-server-ip:8001/docs`
- **Backend Health**: `http://your-server-ip:8001/api/`

## 🔍 Testing Your Deployment

Run the included test script:
```bash
./test-deployment.sh
```

This will verify:
- ✅ All containers are running
- ✅ Backend API is responding
- ✅ Frontend is accessible
- ✅ Database connection works
- ✅ Nginx proxy is routing correctly

## 🛡️ Security Configuration

### For Production Deployment

1. **Change Default Ports** (Optional):
   Edit `docker-compose.prod.yml` to use custom ports:
   ```yaml
   nginx:
     ports:
       - "8080:80"  # Change from default port 80
   ```

2. **Use HTTPS** (Recommended):
   - Add SSL certificates to nginx configuration
   - Update `nginx/nginx.conf` for HTTPS
   - Redirect HTTP to HTTPS

3. **Environment Variables**:
   Create a `.env` file for sensitive data:
   ```env
   MONGO_INITDB_ROOT_USERNAME=admin
   MONGO_INITDB_ROOT_PASSWORD=your_secure_password
   ```

4. **Firewall Configuration**:
   ```bash
   # Allow only necessary ports
   sudo ufw allow 80
   sudo ufw allow 443
   sudo ufw enable
   ```

## 📊 Database Management

### Backup Database
```bash
# Create backup
docker-compose exec mongodb mongodump --out /backup/$(date +%Y%m%d_%H%M%S)

# Copy backup to host
docker cp $(docker-compose ps -q mongodb):/backup ./mongodb-backup
```

### Restore Database
```bash
# Copy backup to container
docker cp ./mongodb-backup $(docker-compose ps -q mongodb):/backup

# Restore from backup
docker-compose exec mongodb mongorestore /backup/your_backup_folder
```

### Connect to Database
```bash
# Access MongoDB shell
docker-compose exec mongodb mongo
```

## 🔧 Customization

### Custom Domain Setup

1. **Update Environment Variables**:
   Edit `frontend/.env.production`:
   ```env
   REACT_APP_BACKEND_URL=https://your-domain.com/api
   ```

2. **Update Nginx Configuration**:
   Edit `nginx/nginx.conf`:
   ```nginx
   server {
       listen 80;
       server_name your-domain.com;
       # ... rest of configuration
   }
   ```

3. **Redeploy**:
   ```bash
   ./deploy.sh --production
   ```

### Performance Optimization

1. **Enable Caching**:
   - Add Redis container for session caching
   - Configure nginx caching for static assets

2. **Resource Limits**:
   Add to docker-compose.yml:
   ```yaml
   backend:
     deploy:
       resources:
         limits:
           memory: 512M
           cpus: '0.5'
   ```

## 🐛 Troubleshooting

### Common Issues

1. **Containers Won't Start**:
   ```bash
   # Check logs
   docker-compose logs

   # Check system resources
   docker system df
   free -h
   ```

2. **Port Already in Use**:
   ```bash
   # Find what's using the port
   sudo netstat -tulpn | grep :80

   # Stop conflicting service
   sudo systemctl stop apache2  # or nginx, if installed
   ```

3. **Database Connection Failed**:
   ```bash
   # Check MongoDB logs
   docker-compose logs mongodb

   # Verify network connectivity
   docker-compose exec backend ping mongodb
   ```

4. **Frontend Not Loading**:
   ```bash
   # Check if build completed successfully
   docker-compose logs frontend

   # Rebuild frontend
   docker-compose up --build -d frontend
   ```

### Reset Everything
```bash
# Stop and remove everything
docker-compose down -v --rmi all

# Clean up Docker system
docker system prune -a

# Start fresh
./deploy.sh
```

## 📈 Monitoring

### Check System Resources
```bash
# Container resource usage
docker stats

# System resource usage
htop
df -h
```

### Log Management
```bash
# Follow live logs
docker-compose logs -f --tail=100

# Check log sizes
docker system df
```

## 🔄 Updates

### Update Application
```bash
# Pull latest changes
git pull origin main

# Rebuild and restart
./deploy.sh --production
```

### Update Docker Images
```bash
# Pull latest base images
docker-compose pull

# Rebuild with latest images
docker-compose up --build -d
```

## 💡 Tips & Best Practices

1. **Regular Backups**: Set up automated MongoDB backups
2. **Monitor Resources**: Keep an eye on disk space and memory usage
3. **Update Regularly**: Keep your OS, Docker, and application updated
4. **Use HTTPS**: Always use SSL/TLS in production
5. **Monitor Logs**: Regularly check application logs for errors

## 🆘 Support

If you encounter issues:

1. Check the troubleshooting section above
2. Run `./test-deployment.sh` to diagnose problems
3. Review logs with `docker-compose logs -f`
4. Check system resources with `docker stats`

## 📄 File Structure

```
retro-os/
├── backend/                 # FastAPI backend
│   ├── server.py           # Main API server
│   ├── requirements.txt    # Python dependencies
│   └── .env               # Backend environment
├── frontend/               # React frontend
│   ├── src/               # React source code
│   ├── package.json       # Node.js dependencies
│   └── .env              # Frontend environment
├── nginx/                 # Nginx configuration
│   ├── nginx.conf        # Main nginx config
│   └── frontend.conf     # Frontend-specific config
├── docker-compose.yml     # Development setup
├── docker-compose.prod.yml # Production setup
├── Dockerfile.backend     # Backend container
├── Dockerfile.frontend    # Frontend container (dev)
├── Dockerfile.frontend.prod # Frontend container (prod)
├── deploy.sh             # Deployment script
├── test-deployment.sh    # Testing script
└── README-Docker.md      # This file
```

---

🎉 **Enjoy your retro computing experience with RetroOS!** 🎉