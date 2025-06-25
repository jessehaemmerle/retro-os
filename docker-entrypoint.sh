#!/bin/bash
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}🚀 Starting RetroOS Docker Deployment${NC}"

# Function to check if a service is ready
check_service() {
    local service_name=$1
    local health_check_url=$2
    local max_attempts=30
    local attempt=1

    echo -e "${YELLOW}⏳ Waiting for $service_name to be ready...${NC}"
    
    while [ $attempt -le $max_attempts ]; do
        if curl -s -f "$health_check_url" >/dev/null 2>&1; then
            echo -e "${GREEN}✅ $service_name is ready!${NC}"
            return 0
        fi
        
        echo -e "${YELLOW}⏳ Attempt $attempt/$max_attempts: $service_name not ready yet...${NC}"
        sleep 2
        ((attempt++))
    done
    
    echo -e "${RED}❌ $service_name failed to start within expected time${NC}"
    return 1
}

# Check if MongoDB is ready
if ! check_service "MongoDB" "mongodb://mongodb:27017"; then
    echo -e "${RED}❌ MongoDB is not available${NC}"
    exit 1
fi

# Check if Backend is ready
if ! check_service "Backend API" "http://backend:8001/api/"; then
    echo -e "${RED}❌ Backend API is not available${NC}"
    exit 1
fi

echo -e "${GREEN}🎉 All services are ready!${NC}"
echo -e "${GREEN}🌐 RetroOS is now accessible at: http://localhost:5050${NC}"
echo -e "${YELLOW}📋 Available endpoints:${NC}"
echo -e "  - Frontend: http://localhost:5050"
echo -e "  - Backend API: http://localhost:5050/api"
echo -e "  - Health Check: http://localhost:5050/health"

# Keep the container running
tail -f /dev/null