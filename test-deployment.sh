#!/bin/bash

# RetroOS Docker Test Script
# This script tests the deployed RetroOS application

echo "🧪 Testing RetroOS Docker Deployment..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

print_testing() {
    echo -e "${YELLOW}🔍 Testing: $1${NC}"
}

# Test if containers are running
print_testing "Container status"
if docker-compose ps | grep -q "Up"; then
    print_success "Containers are running"
else
    print_error "Some containers are not running"
    docker-compose ps
fi

# Test backend API
print_testing "Backend API health"
if curl -f -s http://localhost:8001/api/ > /dev/null; then
    print_success "Backend API is responsive"
else
    print_error "Backend API is not responding"
fi

# Test frontend
print_testing "Frontend availability"
if curl -f -s http://localhost:3000 > /dev/null; then
    print_success "Frontend is accessible"
else
    print_error "Frontend is not accessible"
fi

# Test nginx proxy
print_testing "Nginx proxy"
if curl -f -s http://localhost/ > /dev/null; then
    print_success "Nginx proxy is working"
else
    print_error "Nginx proxy is not working"
fi

# Test MongoDB connection
print_testing "MongoDB connection"
if docker-compose exec -T mongodb mongo --eval "db.runCommand('ping')" > /dev/null 2>&1; then
    print_success "MongoDB is accessible"
else
    print_error "MongoDB connection failed"
fi

echo ""
echo "🎯 Test Summary:"
echo "   • If all tests passed, your RetroOS application is ready!"
echo "   • Access it at: http://localhost"
echo "   • View logs with: docker-compose logs -f"
echo ""