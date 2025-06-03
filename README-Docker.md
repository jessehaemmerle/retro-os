# RetroOS - Docker Deployment Guide

This guide will help you easily deploy the RetroOS application on your Ubuntu VPS using Docker.

## Prerequisites

### 1. Install Docker
```bash
# Update package list
sudo apt update

# Install required packages
sudo apt install apt-transport-https ca-certificates curl software-properties-common

# Add Docker's official GPG key
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg

# Add Docker repository
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

# Install Docker
sudo apt update
sudo apt install docker-ce docker-ce-cli containerd.io docker-compose-plugin

# Add your user to docker group (to run docker without sudo)
sudo usermod -aG docker $USER

# Log out and log back in for group changes to take effect
```

### 2. Install Docker Compose (if not included with Docker)
```bash
# Download Docker Compose
sudo curl -L "https://github.com/docker/compose/releases/download/v2.24.0/docker-compose-$(uname -s)-$(uname -m)" -o /usr/local/bin/docker-compose

# Make it executable
sudo chmod +x /usr/local/bin/docker-compose

# Verify installation
docker-compose --version
```

## Quick Deployment

### Option 1: Automated Deployment (Recommended)
```bash
# Clone your repository
git clone <your-repo-url>
cd <your-repo-directory>

# Make the deployment script executable
chmod +x deploy.sh

# Run the deployment script
./deploy.sh
```

### Option 2: Manual Deployment
```bash
# Clone your repository
git clone <your-repo-url>
cd <your-repo-directory>

# Build and start all services
docker-compose up --build -d

# Check status
docker-compose ps
```

## Configuration

### Environment Variables
The application uses the following key environment variables:

**Backend (.env in backend directory):**
```env
MONGO_URL=mongodb://mongodb:27017
DB_NAME=retroos
```

**Frontend (.env in frontend directory):**
```env
REACT_APP_BACKEND_URL=http://localhost:8001
```

### Custom Domain Setup
If you want to use a custom domain instead of localhost:

1. Update the nginx configuration in `nginx/nginx.conf`
2. Change the `server_name` from `localhost` to your domain
3. Update `REACT_APP_BACKEND_URL` to use your domain
4. Restart the services: `docker-compose restart`

## Service Management

### Start Services
```bash
docker-compose up -d
```

### Stop Services
```bash
docker-compose down
```

### Restart Services
```bash
docker-compose restart
```

### View Logs
```bash
# All services
docker-compose logs -f

# Specific service
docker-compose logs -f backend
docker-compose logs -f frontend
docker-compose logs -f mongodb
```

### Rebuild and Restart
```bash
docker-compose up --build -d
```

## Application Access

Once deployed, you can access the application at:

- **Main Application**: http://your-server-ip/ (or http://localhost/ if testing locally)
- **Frontend Only**: http://your-server-ip:3000
- **Backend API**: http://your-server-ip:8001/api

## Architecture

The Docker setup includes:

1. **Frontend Container** (React)
   - Runs on port 3000
   - Serves the RetroOS interface
   - Hot reload enabled for development

2. **Backend Container** (FastAPI)
   - Runs on port 8001
   - Provides API endpoints
   - Auto-reload enabled for development

3. **MongoDB Container**
   - Runs on port 27017
   - Persistent data storage
   - Automatic database initialization

4. **Nginx Container** (Reverse Proxy)
   - Runs on port 80
   - Routes requests to appropriate services
   - Handles static file serving

## Troubleshooting

### Service Won't Start
```bash
# Check logs for errors
docker-compose logs [service-name]

# Check if ports are already in use
sudo netstat -tulpn | grep :80
sudo netstat -tulpn | grep :3000
sudo netstat -tulpn | grep :8001
```

### Database Connection Issues
```bash
# Check MongoDB logs
docker-compose logs mongodb

# Connect to MongoDB container
docker-compose exec mongodb mongo
```

### Rebuild Specific Service
```bash
# Rebuild just the backend
docker-compose up --build -d backend

# Rebuild just the frontend
docker-compose up --build -d frontend
```

### Reset Everything
```bash
# Stop and remove all containers, networks, and volumes
docker-compose down -v

# Remove all images
docker-compose down --rmi all

# Start fresh
docker-compose up --build -d
```

## Production Considerations

### Security
- Change default MongoDB credentials
- Use HTTPS with SSL certificates
- Configure firewall rules
- Regular security updates

### Performance
- Use production builds for React
- Enable Nginx caching
- Configure resource limits
- Monitor container resource usage

### Backup
```bash
# Backup MongoDB data
docker-compose exec mongodb mongodump --out /backup
docker cp $(docker-compose ps -q mongodb):/backup ./mongodb-backup

# Restore MongoDB data
docker cp ./mongodb-backup $(docker-compose ps -q mongodb):/backup
docker-compose exec mongodb mongorestore /backup
```

## Support

If you encounter any issues:

1. Check the logs: `docker-compose logs -f`
2. Verify all containers are running: `docker-compose ps`
3. Ensure all required ports are available
4. Check firewall settings on your VPS

For additional help, refer to the main README.md file or create an issue in the repository.