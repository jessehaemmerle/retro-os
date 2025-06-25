#!/bin/bash
set -e

echo "RetroOS Application Testing Script"
echo "=================================="

# Test configuration
BACKEND_URL="http://localhost:8001"
FRONTEND_URL="http://localhost:3000"
API_BASE="$BACKEND_URL/api"

# Test functions
test_service() {
    local service_name=$1
    local url=$2
    local expected_status=${3:-200}
    
    echo -n "Testing $service_name... "
    
    if response=$(curl -s -w "%{http_code}" "$url" -o /tmp/response); then
        status_code="${response: -3}"
        if [[ "$status_code" == "$expected_status" ]]; then
            echo "PASS"
            return 0
        else
            echo "FAIL (Status: $status_code)"
            return 1
        fi
    else
        echo "FAIL (Connection error)"
        return 1
    fi
}

test_api_endpoint() {
    local endpoint=$1
    local method=${2:-GET}
    local expected_status=${3:-200}
    local data=${4:-""}
    
    local url="$API_BASE$endpoint"
    
    echo -n "Testing API $method $endpoint... "
    
    if [[ "$method" == "POST" && -n "$data" ]]; then
        response=$(curl -s -w "%{http_code}" -X POST -H "Content-Type: application/json" -d "$data" "$url" -o /tmp/response)
    else
        response=$(curl -s -w "%{http_code}" -X "$method" "$url" -o /tmp/response)
    fi
    
    status_code="${response: -3}"
    if [[ "$status_code" == "$expected_status" ]]; then
        echo "PASS"
        return 0
    else
        echo "FAIL (Status: $status_code)"
        if [[ -f /tmp/response ]]; then
            echo "Response: $(cat /tmp/response)"
        fi
        return 1
    fi
}

# Main testing flow
main() {
    local failed_tests=0
    local total_tests=0
    
    echo ""
    echo "Testing Basic Connectivity"
    echo "=========================="
    
    # Test basic service connectivity
    ((total_tests++))
    test_service "Backend Health" "$BACKEND_URL/api/" || ((failed_tests++))
    
    ((total_tests++))
    test_service "Frontend" "$FRONTEND_URL" || ((failed_tests++))
    
    echo ""
    echo "Testing Backend API Endpoints"
    echo "============================="
    
    # Test all API endpoints
    ((total_tests++))
    test_api_endpoint "/" || ((failed_tests++))
    
    ((total_tests++))
    test_api_endpoint "/wallpapers" || ((failed_tests++))
    
    ((total_tests++))
    test_api_endpoint "/system-info" || ((failed_tests++))
    
    # Test user registration with unique username
    local test_username="testuser$(date +%s)"
    ((total_tests++))
    test_api_endpoint "/register" "POST" 200 "{\"username\":\"$test_username\",\"password\":\"testpass123\"}" || ((failed_tests++))
    
    # Test user login
    ((total_tests++))
    test_api_endpoint "/login" "POST" 200 "{\"username\":\"$test_username\",\"password\":\"testpass123\"}" || ((failed_tests++))
    
    echo ""
    echo "Test Results"
    echo "============"
    
    local passed_tests=$((total_tests - failed_tests))
    echo "Total Tests: $total_tests"
    echo "Passed: $passed_tests"
    echo "Failed: $failed_tests"
    
    if [[ $failed_tests -eq 0 ]]; then
        echo ""
        echo "🎉 All tests passed! RetroOS is working correctly."
        echo "🌐 Access the application at: $FRONTEND_URL"
        return 0
    else
        echo ""
        echo "❌ Some tests failed. Please check the issues above."
        return 1
    fi
}

# Run main function
main

# Cleanup
rm -f /tmp/response